#include "randomizer_logic_tracker.h"

#include "location_access.h"
#include "entrance.h"
#include "../../OTRGlobals.h"
#include "logic_expression.h"
#include "randomizerEnumStrings.h"

#include <algorithm>
#include <array>
#include <deque>
#include <stack>
#include <ranges>
#include <stdexcept>
#include <utility>

#include "logic_tracker.gen.h"

class LogicStateGuard {
  public:
    LogicStateGuard() : isChild(logic->IsChild), isAdult(logic->IsAdult), atDay(logic->AtDay), atNight(logic->AtNight) {
    }

    ~LogicStateGuard() {
        logic->IsChild = isChild;
        logic->IsAdult = isAdult;
        logic->AtDay = atDay;
        logic->AtNight = atNight;
    }

  private:
    bool isChild;
    bool isAdult;
    bool atDay;
    bool atNight;
};

static std::string ToString(const value_t& value) {
    return std::visit(
        [](auto v) -> std::string {
            using V = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<V, bool>) {
                return v ? "true" : "false";
            } else if constexpr (std::is_same_v<V, condition_t>) {
                return "";
            } else if constexpr (std::is_same_v<V, EnumValue>) {
                return std::string(v.Name);
            } else {
                return std::to_string(v);
            }
        },
        value);
}

static std::string ToString(const std::optional<value_t>& value) {
    if (!value.has_value()) {
        return "";
    }
    return ToString(value.value());
}

struct ExpressionTable {
    struct ExpressionRow {
        const LogicExpression* Expression = nullptr;
        std::optional<std::vector<std::pair<ImVec4, std::string>>> DrawingSegments;
        std::vector<ExpressionRow> Children;
        std::vector<ExpressionRow> ConditionBodies;
        std::optional<value_t> ChildDay;
        std::optional<value_t> ChildNight;
        std::optional<value_t> AdultDay;
        std::optional<value_t> AdultNight;
        bool ChildrenInitialized = false;
        bool Expanded = false;
        std::string ErrorMessage;
    };

    ExpressionRow Root;
    bool CombineAll = false;
    bool CombineChild = false;
    bool CombineAdult = false;
};

struct LogicTrackerNode {
    struct Connection {
        std::string ParentName;
        RandomizerRegion ParentRandomizerRegion = RR_NONE;
        ExpressionTable ExpressionTable;

        bool ChildDayAccess = false;
        bool ChildNightAccess = false;
        bool AdultDayAccess = false;
        bool AdultNightAccess = false;
        bool IsDiscovered = true;
    };

    std::string NodeName;
    std::vector<Connection> Connections;
    size_t NodeId = -1;
    RandomizerRegion RandomizerRegion = RR_NONE;
    bool IsExample = false;
};

struct RandomizerEventInfo {
    std::string ConditionStr;
    RandomizerRegion RandomizerRegion = RR_NONE;
};

struct RegionAccessFlags {
    bool ChildDay = false;
    bool ChildNight = false;
    bool AdultDay = false;
    bool AdultNight = false;
};

static bool ApplyTimePassAccess(const Region* region, RegionAccessFlags& access) {
    if (!region->TimePass()) {
        return false;
    }

    bool updated = false;
    if ((access.ChildDay || access.ChildNight) && !(access.ChildDay && access.ChildNight)) {
        access.ChildDay = true;
        access.ChildNight = true;
        updated = true;
    }

    if ((access.AdultDay || access.AdultNight) && !(access.AdultDay && access.AdultNight)) {
        access.AdultDay = true;
        access.AdultNight = true;
        updated = true;
    }

    return updated;
}

static std::vector<RegionAccessFlags> CalculateAccessFromRoot(RandomizerRegion excludedRegion) {
    std::vector<RegionAccessFlags> access(RR_MAX);
    if (excludedRegion == RR_ROOT) {
        return access;
    }

    LogicStateGuard logicStateGuard;

    std::deque<RandomizerRegion> queue;
    const Region* root = RegionTable(RR_ROOT);
    access[RR_ROOT] = { root->childDay, root->childNight, root->adultDay, root->adultNight };
    queue.push_back(RR_ROOT);

    while (!queue.empty()) {
        RandomizerRegion regionKey = queue.front();
        queue.pop_front();

        Region* region = RegionTable(regionKey);
        logic->CurrentRegionKey = regionKey;
        const RegionAccessFlags currentAccess = access[regionKey];

        for (auto& exit : region->exits) {
            RandomizerRegion connectedKey = exit.GetConnectedRegionKey();
            if (connectedKey == RR_NONE || connectedKey == excludedRegion) {
                continue;
            }

            Region* connectedRegion = RegionTable(connectedKey);
            RegionAccessFlags& targetAccess = access[connectedKey];
            bool updated = false;

            if (currentAccess.ChildDay && exit.CheckConditionAtAgeTime(logic->IsChild, logic->AtDay) &&
                !targetAccess.ChildDay) {
                targetAccess.ChildDay = true;
                updated = true;
            }
            if (currentAccess.ChildNight && exit.CheckConditionAtAgeTime(logic->IsChild, logic->AtNight) &&
                !targetAccess.ChildNight) {
                targetAccess.ChildNight = true;
                updated = true;
            }
            if (currentAccess.AdultDay && exit.CheckConditionAtAgeTime(logic->IsAdult, logic->AtDay) &&
                !targetAccess.AdultDay) {
                targetAccess.AdultDay = true;
                updated = true;
            }
            if (currentAccess.AdultNight && exit.CheckConditionAtAgeTime(logic->IsAdult, logic->AtNight) &&
                !targetAccess.AdultNight) {
                targetAccess.AdultNight = true;
                updated = true;
            }

            updated |= ApplyTimePassAccess(connectedRegion, targetAccess);

            if (updated) {
                queue.push_back(connectedKey);
            }
        }
    }

    logic->CurrentRegionKey = RR_NONE;
    return access;
}

std::unordered_map<std::string, std::vector<RandomizerEventInfo>> randomizerEventMap;

bool expandingNode = false;
size_t expandNodeId = -1;
size_t clearNodesAfterNodeID = -1;
std::vector<LogicTrackerNode> nodes;

static ExpressionTable::ExpressionRow CreateExpressionRows(const LogicExpression* expression) {
    expression = expression->GetDisplayExpression();
    ExpressionTable::ExpressionRow row;
    row.Expression = expression;
    row.Expanded = false;
    return row;
}

static void EnsureExpressionChildren(ExpressionTable::ExpressionRow& row) {
    if (row.ChildrenInitialized || !row.Expression) {
        return;
    }
    if (!dynamic_cast<const ConditionExpression*>(row.Expression)) {
        const auto children = row.Expression->GetChildren();
        std::vector<ExpressionTable::ExpressionRow> rows;
        rows.reserve(children.size());
        for (const auto* child : children) {
            rows.emplace_back(CreateExpressionRows(child));
        }
        row.Children = std::move(rows);
    }
    row.ChildrenInitialized = true;
}

enum class AgeTime { ChildDay, ChildNight, AdultDay, AdultNight };

static void PopulateExpressionValues(ExpressionTable::ExpressionRow& row, const EvalResult& eval, AgeTime ageTime) {
    EnsureExpressionChildren(row);
    const auto& invocation = eval.Invocation;
    const auto& displayed = std::holds_alternative<condition_t>(eval.Result) && invocation ? *invocation : eval;
    std::optional<value_t> value;
    if (!std::holds_alternative<condition_t>(displayed.Result)) {
        value = displayed.Result;
    }
    if (ageTime == AgeTime::ChildDay) {
        row.ChildDay = value;
    } else if (ageTime == AgeTime::ChildNight) {
        row.ChildNight = value;
    } else if (ageTime == AgeTime::AdultDay) {
        row.AdultDay = value;
    } else if (ageTime == AgeTime::AdultNight) {
        row.AdultNight = value;
    }

    for (const auto& child : eval.Children) {
        assert(child.ChildIndex < row.Children.size());
        auto& rowChild = row.Children.at(child.ChildIndex);
        assert(rowChild.Expression == child.Expression->GetDisplayExpression());
        PopulateExpressionValues(rowChild, child, ageTime);
    }

    if (invocation) {
        auto it = std::find_if(row.ConditionBodies.begin(), row.ConditionBodies.end(), [&](const auto& existing) {
            return existing.Expression == invocation->Expression->GetDisplayExpression();
        });
        if (it == row.ConditionBodies.end()) {
            row.ConditionBodies.push_back(CreateExpressionRows(invocation->Expression));
            it = std::prev(row.ConditionBodies.end());
        }
        PopulateExpressionValues(*it, *invocation, ageTime);
    }
}

static std::tuple<bool, bool, bool> CalculateCombines(const ExpressionTable::ExpressionRow& row) {
    bool combineChild = row.ChildDay == row.ChildNight;
    bool combineAdult = row.AdultDay == row.AdultNight;
    bool combineAll = combineChild && combineAdult && row.ChildDay == row.AdultDay;
    for (const auto& child : row.Children) {
        auto [childCombineAll, childCombineChild, childCombineAdult] = CalculateCombines(child);
        combineAll &= childCombineAll;
        combineChild &= childCombineChild;
        combineAdult &= childCombineAdult;
    }
    for (const auto& body : row.ConditionBodies) {
        auto [bodyCombineAll, bodyCombineChild, bodyCombineAdult] = CalculateCombines(body);
        combineAll &= bodyCombineAll;
        combineChild &= bodyCombineChild;
        combineAdult &= bodyCombineAdult;
    }
    return { combineAll, combineChild, combineAdult };
}

static void PopulateConnectionExpression(LogicTrackerNode::Connection& connection, const LogicRootKey& key) {
    LogicStateGuard logicStateGuard;

    try {
        static const LogicRootRegistry roots(GetLogicRoots(), GetExpressionCount());
        const auto* expression = roots.GetExpression(key);
        if (!expression) {
            throw std::runtime_error("No tracker root registered for " + std::string(key.OwnerRegion) + " / " +
                                     std::string(key.Entry));
        }
        logic->IsChild = false;
        logic->IsAdult = false;
        logic->AtDay = false;
        logic->AtNight = false;

        connection.ExpressionTable.Root = CreateExpressionRows(expression);

        if (connection.ChildDayAccess) {
            logic->IsChild = true;
            logic->AtDay = true;

            VariableScope scope;
            const auto& eval = expression->Evaluate(scope);
            PopulateExpressionValues(connection.ExpressionTable.Root, eval, AgeTime::ChildDay);

            logic->IsChild = false;
            logic->AtDay = false;
        }
        if (connection.ChildNightAccess) {
            logic->IsChild = true;
            logic->AtNight = true;

            VariableScope scope;
            const auto& eval = expression->Evaluate(scope);
            PopulateExpressionValues(connection.ExpressionTable.Root, eval, AgeTime::ChildNight);

            logic->IsChild = false;
            logic->AtNight = false;
        }
        if (connection.AdultDayAccess) {
            logic->IsAdult = true;
            logic->AtDay = true;

            VariableScope scope;
            const auto& eval = expression->Evaluate(scope);
            PopulateExpressionValues(connection.ExpressionTable.Root, eval, AgeTime::AdultDay);

            logic->IsAdult = false;
            logic->AtDay = false;
        }
        if (connection.AdultNightAccess) {
            logic->IsAdult = true;
            logic->AtNight = true;

            VariableScope scope;
            const auto& eval = expression->Evaluate(scope);
            PopulateExpressionValues(connection.ExpressionTable.Root, eval, AgeTime::AdultNight);

            logic->IsAdult = false;
            logic->AtNight = false;
        }
    } catch (const std::exception& e) {
        connection.ExpressionTable.Root.ErrorMessage = std::string("Eval Error: ") + e.what();
        connection.ExpressionTable.Root.Children.clear();
        connection.ExpressionTable.Root.ConditionBodies.clear();
        connection.ExpressionTable.CombineAll = true;
        return;
    }

    auto [combineAll, combineChild, combineAdult] = CalculateCombines(connection.ExpressionTable.Root);
    connection.ExpressionTable.CombineAll = combineAll;
    connection.ExpressionTable.CombineChild = combineChild;
    connection.ExpressionTable.CombineAdult = combineAdult;
}

RandomizerCheck showRandomizerCheck = RC_UNKNOWN_CHECK;

void LogicTrackerWindow::ShowRandomizerCheck(RandomizerCheck randomizerCheck) {
    showRandomizerCheck = randomizerCheck;

    auto window = Ship::Context::GetInstance()->GetWindow()->GetGui()->GetGuiWindow("Logic Tracker");
    window->Show();
    ImGui::SetWindowFocus(window->GetName().c_str());
}

static void CalculateShowRandomizerCheck() {
    if (showRandomizerCheck == RC_UNKNOWN_CHECK) {
        return;
    }
    logic->CurrentCheckKey = showRandomizerCheck;

    const auto& location = Rando::StaticData::GetLocation(showRandomizerCheck);
    nodes.clear();

    LogicTrackerNode node;
    node.NodeName = "Check: " + location->GetName();
    node.NodeId = nodes.size();

    for (int randomizerRegion = RR_NONE; randomizerRegion < RR_MAX; ++randomizerRegion) {
        const auto& region = areaTable[randomizerRegion];
        for (const auto& locationAccess : region.locations) {
            if (locationAccess.GetLocation() == showRandomizerCheck) {
                logic->CurrentRegionKey = RandomizerRegion(randomizerRegion);

                LogicTrackerNode::Connection connection;
                connection.ParentName = "Region: " + region.regionName;
                connection.ParentRandomizerRegion = RandomizerRegion(randomizerRegion);
                connection.ChildDayAccess = region.childDay;
                connection.ChildNightAccess = region.childNight;
                connection.AdultDayAccess = region.adultDay;
                connection.AdultNightAccess = region.adultNight;

                PopulateConnectionExpression(connection,
                                             { LogicRootKind::Location,
                                               EnumToString(RandomizerRegion(randomizerRegion)).value_or(""),
                                               EnumToString(showRandomizerCheck).value_or("") });

                node.Connections.emplace_back(std::move(connection));
                logic->CurrentRegionKey = RR_NONE;
            }
        }
    }

    nodes.emplace_back(std::move(node));

    expandNodeId = node.NodeId;

    logic->CurrentCheckKey = RC_UNKNOWN_CHECK;
    showRandomizerCheck = RC_UNKNOWN_CHECK;
}

RandomizerRegion showToRandomizerRegion = RR_NONE;
RandomizerRegion showFromRandomizerRegion = RR_NONE;

void LogicTrackerWindow::ShowRandomizerRegion(RandomizerRegion toRandomizerRegion,
                                              RandomizerRegion fromRandomizerRegion) {
    showToRandomizerRegion = toRandomizerRegion;
    showFromRandomizerRegion = fromRandomizerRegion;
}

static void CalculateShowRandomizerRegion() {
    if (showToRandomizerRegion == RR_NONE) {
        return;
    }
    logic->CurrentCheckKey = RC_UNKNOWN_CHECK;

    const auto& region = RegionTable(showToRandomizerRegion);

    LogicTrackerNode node;
    node.NodeName = "Region: " + region->regionName;
    node.NodeId = nodes.size();
    node.RandomizerRegion = showToRandomizerRegion;

    const auto accessFromRoot = CalculateAccessFromRoot(showToRandomizerRegion);

    for (const auto& entrance : region->entrances) {
        if (entrance->GetParentRegionKey() == showFromRandomizerRegion) {
            continue;
        }
        logic->CurrentRegionKey = entrance->GetParentRegionKey();

        const auto& parentRegion = entrance->GetParentRegion();

        LogicTrackerNode::Connection connection;
        connection.ParentName = "Region: " + parentRegion->regionName;
        if (entrance->IsShuffled()) {
            const auto& originalConnectedRegion = RegionTable(entrance->GetOriginalConnectedRegionKey());
            connection.ParentName += ",    Originally to: " + originalConnectedRegion->regionName;
            connection.IsDiscovered =
                Entrance_GetIsEntranceDiscovered(entrance->GetIndex()) || logic->ACProcessUndiscoveredExits;
        }
        connection.ParentRandomizerRegion = entrance->GetParentRegionKey();

        const auto& parentAccess = accessFromRoot[connection.ParentRandomizerRegion];
        connection.ChildDayAccess = parentAccess.ChildDay;
        connection.ChildNightAccess = parentAccess.ChildNight;
        connection.AdultDayAccess = parentAccess.AdultDay;
        connection.AdultNightAccess = parentAccess.AdultNight;

        PopulateConnectionExpression(connection,
                                     { LogicRootKind::Exit, EnumToString(entrance->GetParentRegionKey()).value_or(""),
                                       EnumToString(entrance->GetOriginalConnectedRegionKey()).value_or("") });

        node.Connections.emplace_back(std::move(connection));
        logic->CurrentRegionKey = RR_NONE;
    }

    nodes.emplace_back(std::move(node));

    expandNodeId = node.NodeId;

    showToRandomizerRegion = RR_NONE;
    showFromRandomizerRegion = RR_NONE;
}

std::string showEvent = "";

void LogicTrackerWindow::ShowRandomizerEvent(const std::string& event) {
    showEvent = event;
}

static void CalculateShowRandomizerEvent() {
    if (showEvent.empty()) {
        return;
    }

    const auto& events = randomizerEventMap.find(showEvent);
    if (events == randomizerEventMap.end()) {
        return;
    }
    logic->CurrentCheckKey = RC_UNKNOWN_CHECK;

    LogicTrackerNode node;
    node.NodeName = "Event: " + showEvent;
    node.NodeId = nodes.size();

    for (const auto& eventInfo : events->second) {
        logic->CurrentRegionKey = eventInfo.RandomizerRegion;
        const auto& region = areaTable[eventInfo.RandomizerRegion];

        LogicTrackerNode::Connection connection;
        connection.ParentName = "Region: " + region.regionName;
        connection.ParentRandomizerRegion = eventInfo.RandomizerRegion;
        connection.ChildDayAccess = region.childDay;
        connection.ChildNightAccess = region.childNight;
        connection.AdultDayAccess = region.adultDay;
        connection.AdultNightAccess = region.adultNight;

        PopulateConnectionExpression(
            connection, { LogicRootKind::Event, EnumToString(eventInfo.RandomizerRegion).value_or(""), showEvent });

        node.Connections.emplace_back(std::move(connection));
        logic->CurrentRegionKey = RR_NONE;
    }

    nodes.emplace_back(std::move(node));

    expandNodeId = node.NodeId;

    showEvent.clear();
}

static bool HasExpressionChildren(ExpressionTable::ExpressionRow& row) {
    if (dynamic_cast<const ParameterExpression*>(row.Expression)) {
        EnsureExpressionChildren(row);
        for (auto& child : row.Children) {
            if (HasExpressionChildren(child)) {
                return true;
            }
        }
    } else if (row.Expression && !dynamic_cast<const ConditionExpression*>(row.Expression) &&
               !row.Expression->GetChildren().empty()) {
        return true;
    }
    for (auto& body : row.ConditionBodies) {
        if (HasExpressionChildren(body)) {
            return true;
        }
    }
    return false;
}

static void DrawExpressionText(ExpressionTable::ExpressionRow& row, size_t startColorIndex) {
    if (!row.DrawingSegments) {
        row.DrawingSegments = row.Expression->GetDrawingSegments(startColorIndex);
    }
    LogicExpression::DrawSegments(*row.DrawingSegments);
}

static void DrawExpressionRow(const LogicTrackerNode& node, const ExpressionTable& table,
                              ExpressionTable::ExpressionRow& row, int level, size_t startColorIndex = 0);

static void DrawExpressionChildren(const LogicTrackerNode& node, const ExpressionTable& table,
                                   ExpressionTable::ExpressionRow& row, int level, size_t startColorIndex) {
    EnsureExpressionChildren(row);
    for (size_t i = 0; i < row.Children.size(); ++i) {
        auto& child = row.Children[i];
        const auto childColorIndex = row.Expression->GetChildColorIndex(i, startColorIndex);
        if (dynamic_cast<const ParameterExpression*>(row.Expression)) {
            DrawExpressionChildren(node, table, child, level, childColorIndex);
        } else {
            DrawExpressionRow(node, table, child, level, childColorIndex);
        }
    }
    for (auto& body : row.ConditionBodies) {
        DrawExpressionChildren(node, table, body, level, startColorIndex);
    }
}

static void DrawExpressionRow(const LogicTrackerNode& node, const ExpressionTable& table,
                              ExpressionTable::ExpressionRow& row, int level, size_t startColorIndex) {
    if (dynamic_cast<const MatchArmPatterns*>(row.Expression) && row.Expression->GetChildren().empty()) {
        return;
    }

    bool hasChildren = false;
    const ValueExpression* eventValue = nullptr;
    if (row.Expression && row.ErrorMessage.empty()) {
        try {
            hasChildren = HasExpressionChildren(row);
            const LogicExpression* operand = row.Expression;
            if (const auto* call = dynamic_cast<const ExternFunctionCallExpression*>(operand);
                call && call->GetFunctionName() == "flag") {
                operand = call->GetChildren()[0]->GetDisplayExpression();
            }
            while (dynamic_cast<const ParameterExpression*>(operand)) {
                operand = operand->GetChildren()[0]->GetDisplayExpression();
            }
            const auto* value = dynamic_cast<const ValueExpression*>(operand);
            if (value && value->IsEvent()) {
                eventValue = value;
            }
        } catch (const std::exception& e) {
            row.ErrorMessage = std::string("Expression Error: ") + e.what();
            row.Expanded = false;
        }
    }

    ImGui::TableNextRow();
    ImGui::PushID(&row);
    ImGui::PushFont(OTRGlobals::Instance->fontMono);
    if (level > 0) {
        ImGui::Indent(20.0f);
    }

    ImGui::TableNextColumn();
    if (!row.Expression) {
        ImGui::BeginDisabled();
        ImGui::Button("- ");
        ImGui::EndDisabled();

        ImGui::TableNextColumn();
        ImGui::TextWrapped("%s", row.ErrorMessage.c_str());

        ImGui::TableNextColumn();
        ImGui::TextUnformatted("");

        if (level > 0) {
            ImGui::Unindent(20.0f);
        }
        ImGui::PopFont();
        ImGui::PopID();
        return;
    }

    if (hasChildren) {
        if (ImGui::Button(std::to_string(level).c_str())) {
            row.Expanded = !row.Expanded;
        }
    } else {
        ImGui::BeginDisabled();
        ImGui::Button(std::to_string(level).c_str());
        ImGui::EndDisabled();
    }

    if (eventValue) {
        std::string identifier = eventValue->ToString();
        auto it = randomizerEventMap.find(identifier);
        if (it != randomizerEventMap.end()) {
            ImGui::SameLine();
            ImGui::BeginDisabled(node.IsExample);
            if (ImGui::Button(ICON_FA_COGS)) {
                LogicTrackerWindow::ShowRandomizerEvent(identifier);
                clearNodesAfterNodeID = node.NodeId;
            }
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::SetTooltip("Show Event Logic");
            }
        }
    }

    ImGui::TableNextColumn();
    if (row.ErrorMessage.empty()) {
        try {
            DrawExpressionText(row, startColorIndex);
        } catch (const std::exception& e) {
            row.ErrorMessage = std::string("Expression Error: ") + e.what();
            row.Expanded = false;
        }
    }
    if (!row.ErrorMessage.empty()) {
        ImGui::TextWrapped("%s", row.ErrorMessage.c_str());
    }

    ImGui::TableNextColumn();
    ImGui::TextUnformatted(ToString(row.ChildDay).c_str());

    if (!table.CombineAll) {
        if (!table.CombineChild) {
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(ToString(row.ChildNight).c_str());
        }

        ImGui::TableNextColumn();
        ImGui::TextUnformatted(ToString(row.AdultDay).c_str());

        if (!table.CombineAdult) {
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(ToString(row.AdultNight).c_str());
        }
    }

    if (row.Expanded && row.ErrorMessage.empty()) {
        try {
            DrawExpressionChildren(node, table, row, level + 1, startColorIndex);
        } catch (const std::exception& e) {
            row.ErrorMessage = std::string("Expression Error: ") + e.what();
            row.Expanded = false;
        }
    }

    if (level > 0) {
        ImGui::Unindent(20.0f);
    }
    ImGui::PopFont();
    ImGui::PopID();
}

static void DrawExpressionTable(const LogicTrackerNode& node, ExpressionTable& table) {
    int columnCount = 3;
    if (!table.CombineAll) {
        if (!table.CombineChild) {
            columnCount += 1;
        }
        columnCount += 1;
        if (!table.CombineAdult) {
            columnCount += 1;
        }
    }

    ImGui::PushID(&table);
    if (ImGui::BeginTable("", columnCount,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("Level", ImGuiTableColumnFlags_IndentEnable | ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("Expression", ImGuiTableColumnFlags_WidthStretch);
        if (table.CombineAll) {
            ImGui::TableSetupColumn("All", ImGuiTableColumnFlags_WidthFixed);
        } else {
            if (table.CombineChild) {
                ImGui::TableSetupColumn("Child", ImGuiTableColumnFlags_WidthFixed);
            } else {
                ImGui::TableSetupColumn("Child Day", ImGuiTableColumnFlags_WidthFixed);
                ImGui::TableSetupColumn("Child Night", ImGuiTableColumnFlags_WidthFixed);
            }
            if (table.CombineAdult) {
                ImGui::TableSetupColumn("Adult", ImGuiTableColumnFlags_WidthFixed);
            } else {
                ImGui::TableSetupColumn("Adult Day", ImGuiTableColumnFlags_WidthFixed);
                ImGui::TableSetupColumn("Adult Night", ImGuiTableColumnFlags_WidthFixed);
            }
        }
        ImGui::TableHeadersRow();

        DrawExpressionRow(node, table, table.Root, 0);

        ImGui::EndTable();
    }
    ImGui::PopID();
}

static void DrawNodeConnection(const LogicTrackerNode& node, LogicTrackerNode::Connection& connection) {
    DrawExpressionTable(node, connection.ExpressionTable);
}

static std::string GetAccessString(const LogicTrackerNode::Connection& connection) {
    std::string access = "";
    if (connection.ChildDayAccess && connection.ChildNightAccess && connection.AdultDayAccess &&
        connection.AdultNightAccess) {
        access += "All";
    } else if (connection.ChildDayAccess || connection.ChildNightAccess || connection.AdultDayAccess ||
               connection.AdultNightAccess) {
        std::vector<std::string> accessParts;
        if (connection.ChildDayAccess && connection.ChildNightAccess) {
            accessParts.push_back("Child");
        } else {
            if (connection.ChildDayAccess) {
                accessParts.push_back("Child Day");
            }
            if (connection.ChildNightAccess) {
                accessParts.push_back("Child Night");
            }
        }
        if (connection.AdultDayAccess && connection.AdultNightAccess) {
            accessParts.push_back("Adult");
        } else {
            if (connection.AdultDayAccess) {
                accessParts.push_back("Adult Day");
            }
            if (connection.AdultNightAccess) {
                accessParts.push_back("Adult Night");
            }
        }
        if (!accessParts.empty()) {
            for (size_t i = 0; i < accessParts.size(); ++i) {
                if (i > 0) {
                    access += ", ";
                }
                access += accessParts[i];
            }
        }
    } else {
        access = "None";
    }
    return access;
}

static std::string GetAvailableString(const LogicTrackerNode::Connection& connection) {
    const auto& expressionRow = connection.ExpressionTable.Root;

    bool childDay = ToBool(expressionRow.ChildDay);
    bool childNight = ToBool(expressionRow.ChildNight);
    bool adultDay = ToBool(expressionRow.AdultDay);
    bool adultNight = ToBool(expressionRow.AdultNight);

    std::string available = "";
    if (childDay && childNight && adultDay && adultNight) {
        available = "All";
    } else if (childDay || childNight || adultDay || adultNight) {
        std::vector<std::string> availableParts;
        if (childDay && childNight) {
            availableParts.push_back("Child");
        } else {
            if (childDay) {
                availableParts.push_back("Child Day");
            }
            if (childNight) {
                availableParts.push_back("Child Night");
            }
        }
        if (adultDay && adultNight) {
            availableParts.push_back("Adult");
        } else {
            if (adultDay) {
                availableParts.push_back("Adult Day");
            }
            if (adultNight) {
                availableParts.push_back("Adult Night");
            }
        }
        if (!availableParts.empty()) {
            for (size_t i = 0; i < availableParts.size(); ++i) {
                if (i > 0) {
                    available += ", ";
                }
                available += availableParts[i];
            }
        }
    } else {
        available = "None";
    }
    return available;
}

static void DrawNode(LogicTrackerNode& node) {
    ImGui::PushID(static_cast<int>(node.NodeId));

    if (node.IsExample) {
        ImGui::SetNextItemOpen(true, ImGuiCond_Once);
    } else if (expandingNode) {
        ImGui::SetNextItemOpen(expandNodeId == node.NodeId, ImGuiCond_Always);
    }

    bool nodeOpen = ImGui::CollapsingHeader(("To " + node.NodeName).c_str(),
                                            ImGuiTreeNodeFlags_AllowOverlap | ImGuiTreeNodeFlags_SpanAvailWidth);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Show Connections");
    }

    if (nodeOpen) {
        ImGui::Indent(25.0f);

        for (int i = 0; i < node.Connections.size(); i++) {
            auto& connection = node.Connections[i];

            if (!connection.IsDiscovered) {
                ImGui::BeginDisabled();
                ImGui::CollapsingHeader("Undiscovered Entrance",
                                        ImGuiTreeNodeFlags_AllowOverlap | ImGuiTreeNodeFlags_SpanAvailWidth);
                ImGui::EndDisabled();
                continue;
            }

            ImGui::PushID(connection.ParentName.c_str());

            if (node.IsExample) {
                ImGui::SetNextItemOpen(true, ImGuiCond_Once);
            } else if (expandingNode && node.Connections.size() == 1) {
                ImGui::SetNextItemOpen(expandNodeId == node.NodeId, ImGuiCond_Always);
            }

            const float connectionRight = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
            bool connectionOpen =
                ImGui::CollapsingHeader(("From " + connection.ParentName).c_str(),
                                        ImGuiTreeNodeFlags_AllowOverlap | ImGuiTreeNodeFlags_SpanAvailWidth);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Show Connection Logic");
            }

            std::string accessAvailable =
                GetAccessString(connection) + " " + ICON_FA_ARROW_RIGHT + " " + GetAvailableString(connection);

            const float gearWidth = ImGui::CalcTextSize(ICON_FA_COGS).x + ImGui::GetStyle().FramePadding.x * 2.0f;
            const float accessX = connectionRight - ImGui::CalcTextSize(accessAvailable.c_str()).x -
                                  ImGui::GetStyle().ItemSpacing.x - gearWidth;
            ImGui::SameLine(std::max(ImGui::GetCursorPosX(), accessX));
            ImGui::TextUnformatted(accessAvailable.c_str());
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Connection Access " ICON_FA_ARROW_RIGHT " Available");
            }

            ImGui::SameLine();
            ImGui::BeginDisabled(node.IsExample);
            if (ImGui::Button(ICON_FA_COGS)) {
                LogicTrackerWindow::ShowRandomizerRegion(connection.ParentRandomizerRegion, node.RandomizerRegion);
                clearNodesAfterNodeID = node.NodeId;
            }
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::SetTooltip("Show Region Logic");
            }

            if (connectionOpen) {
                DrawNodeConnection(node, connection);
            }

            ImGui::PopID();
        }

        ImGui::Unindent(25.0f);
    }

    ImGui::PopID();
}

static void PopulateRandomizerEventMap() {
    if (!randomizerEventMap.empty()) {
        return;
    }
    for (int i = RR_NONE; i < RR_MAX; i++) {
        for (const auto& event : areaTable[i].events) {
            const auto& eventStr = event.GetEventStr();
            auto it = randomizerEventMap.find(eventStr);
            auto eventInfo = RandomizerEventInfo{ event.GetConditionStr(), RandomizerRegion(i) };
            if (it != randomizerEventMap.end()) {
                it->second.emplace_back(std::move(eventInfo));
            } else {
                randomizerEventMap.emplace(eventStr, std::vector<RandomizerEventInfo>{ std::move(eventInfo) });
            }
        }
    }
}

void LogicTrackerWindow::DrawElement() {
    if (expandNodeId != -1) {
        expandingNode = true;
    }

    for (LogicTrackerNode& node : nodes) {
        DrawNode(node);
    }

    if (expandingNode) {
        expandNodeId = -1;
        expandingNode = false;
    }

    if (clearNodesAfterNodeID != -1) {
        while (nodes.back().NodeId > clearNodesAfterNodeID) {
            nodes.pop_back();
        }
        clearNodesAfterNodeID = -1;
    }
}

void LogicTrackerWindow::InitElement() {
}

void LogicTrackerWindow::UpdateElement() {
    PopulateRandomizerEventMap();
    CalculateShowRandomizerCheck();
    CalculateShowRandomizerRegion();
    CalculateShowRandomizerEvent();
}

static LogicExpression* GetExampleExpressions(index_t index) {
    static std::unordered_map<index_t, std::unique_ptr<LogicExpression>> cache;
    auto cacheResult = cache.find(index);
    if (cacheResult != cache.end()) {
        return cacheResult->second.get();
    }

    // clang-format off
    auto createExpression = [](index_t index) -> std::unique_ptr<LogicExpression> {
        auto empty = [](std::span<EvalResult>) -> value_t { throw std::logic_error("User guide results are supplied explicitly."); };
        switch (index) {
            case 0: return std::make_unique<AndExpression>(std::vector<index_t>{1, 2});
            case 1: return std::make_unique<ExternFunctionCallExpression>(empty, "is_child", std::vector<index_t>{});
            case 2: return std::make_unique<ExternFunctionCallExpression>(empty, "can_use", std::vector<index_t>{3});
            case 3: return std::make_unique<ParameterExpression>(0, "item", 4);
            case 4: return std::make_unique<ValueExpression>([](VariableScope&) -> value_t { return EnumValue{RG_SONG_OF_STORMS, "RG_SONG_OF_STORMS"}; }, "RG_SONG_OF_STORMS", false);

            case 5: return std::make_unique<OrExpression>(std::vector<index_t>{6, 9});
            case 6: return std::make_unique<ExternFunctionCallExpression>(empty, "has", std::vector<index_t>{7});
            case 7: return std::make_unique<ParameterExpression>(0, "item", 8);
            case 8: return std::make_unique<ValueExpression>([](VariableScope&) -> value_t { return EnumValue{RG_SILVER_SCALE, "RG_SILVER_SCALE"}; }, "RG_SILVER_SCALE", false);
            case 9: return std::make_unique<ExternFunctionCallExpression>(empty, "can_use", std::vector<index_t>{10});
            case 10: return std::make_unique<ParameterExpression>(0, "item", 11);
            case 11: return std::make_unique<ValueExpression>([](VariableScope&) -> value_t { return EnumValue{RG_IRON_BOOTS, "RG_IRON_BOOTS"}; }, "RG_IRON_BOOTS", false);

            case 12: return std::make_unique<ExternFunctionCallExpression>(empty, "has", std::vector<index_t>{13});
            case 13: return std::make_unique<ParameterExpression>(0, "item", 14);
            case 14: return std::make_unique<ValueExpression>([](VariableScope&) -> value_t { return EnumValue{RG_POWER_BRACELET, "RG_POWER_BRACELET"}; }, "RG_POWER_BRACELET", false);

            case 15: return std::make_unique<OrExpression>(std::vector<index_t>{16, 17});
            case 16: return std::make_unique<AndExpression>(std::vector<index_t>{1, 12});
            case 17: return std::make_unique<ExternFunctionCallExpression>(empty, "can_use", std::vector<index_t>{18});
            case 18: return std::make_unique<ParameterExpression>(0, "item", 19);
            case 19: return std::make_unique<ValueExpression>([](VariableScope&) -> value_t { return EnumValue{RG_HOVER_BOOTS, "RG_HOVER_BOOTS"}; }, "RG_HOVER_BOOTS", false);

            case 20: return std::make_unique<ExternFunctionCallExpression>(empty, "any_age", std::vector<index_t>{21});
            case 21: return std::make_unique<ParameterExpression>(0, "condition", 22);
            case 22: return std::make_unique<ConditionExpression>(23);
            case 23: return std::make_unique<FunctionCallExpression>("blast_or_smash", std::vector<index_t>{}, 24);
            case 24: return std::make_unique<OrExpression>(std::vector<index_t>{25, 26});
            case 25: return std::make_unique<ExternFunctionCallExpression>(empty, "has_explosives", std::vector<index_t>{});
            case 26: return std::make_unique<ExternFunctionCallExpression>(empty, "can_use", std::vector<index_t>{27});
            case 27: return std::make_unique<ParameterExpression>(0, "item", 28);
            case 28: return std::make_unique<ValueExpression>([](VariableScope&) -> value_t { return EnumValue{RG_MEGATON_HAMMER, "RG_MEGATON_HAMMER"}; }, "RG_MEGATON_HAMMER", false);

            case 29: return std::make_unique<MatchExpression<bool>>(30, std::vector<index_t>{31, 43, 54});
            case 30: return std::make_unique<ValueExpression>([](VariableScope&) -> value_t { return EnumValue{RG_GANON_SOUL, "RG_GANON_SOUL"}; }, "item", false);
            case 31: return std::make_unique<MatchArm>(32, 41, false);
            case 32: return std::make_unique<MatchArmPatterns>(std::vector<index_t>{33, 34, 35, 36, 37, 38, 39, 40});
            case 33: return std::make_unique<ValueExpression>([](VariableScope&) -> value_t { return EnumValue{RG_GOHMA_SOUL, "RG_GOHMA_SOUL"}; }, "RG_GOHMA_SOUL", false);
            case 34: return std::make_unique<ValueExpression>([](VariableScope&) -> value_t { return EnumValue{RG_KING_DODONGO_SOUL, "RG_KING_DODONGO_SOUL"}; }, "RG_KING_DODONGO_SOUL", false);
            case 35: return std::make_unique<ValueExpression>([](VariableScope&) -> value_t { return EnumValue{RG_BARINADE_SOUL, "RG_BARINADE_SOUL"}; }, "RG_BARINADE_SOUL", false);
            case 36: return std::make_unique<ValueExpression>([](VariableScope&) -> value_t { return EnumValue{RG_PHANTOM_GANON_SOUL, "RG_PHANTOM_GANON_SOUL"}; }, "RG_PHANTOM_GANON_SOUL", false);
            case 37: return std::make_unique<ValueExpression>([](VariableScope&) -> value_t { return EnumValue{RG_VOLVAGIA_SOUL, "RG_VOLVAGIA_SOUL"}; }, "RG_VOLVAGIA_SOUL", false);
            case 38: return std::make_unique<ValueExpression>([](VariableScope&) -> value_t { return EnumValue{RG_MORPHA_SOUL, "RG_MORPHA_SOUL"}; }, "RG_MORPHA_SOUL", false);
            case 39: return std::make_unique<ValueExpression>([](VariableScope&) -> value_t { return EnumValue{RG_BONGO_BONGO_SOUL, "RG_BONGO_BONGO_SOUL"}; }, "RG_BONGO_BONGO_SOUL", false);
            case 40: return std::make_unique<ValueExpression>([](VariableScope&) -> value_t { return EnumValue{RG_TWINROVA_SOUL, "RG_TWINROVA_SOUL"}; }, "RG_TWINROVA_SOUL", false);
            case 41: return std::make_unique<ExternFunctionCallExpression>(empty, "has", std::vector<index_t>{42});
            case 42: return std::make_unique<ParameterExpression>(0, "item", 30);
            case 43: return std::make_unique<MatchArm>(44, 46, false);
            case 44: return std::make_unique<MatchArmPatterns>(std::vector<index_t>{45});
            case 45: return std::make_unique<ValueExpression>([](VariableScope&) -> value_t { return EnumValue{RG_GANON_SOUL, "RG_GANON_SOUL"}; }, "RG_GANON_SOUL", false);
            case 46: return std::make_unique<OrExpression>(std::vector<index_t>{47, 52});
            case 47: return std::make_unique<ComparisonExpression>("!=", 48, 51);
            case 48: return std::make_unique<ExternFunctionCallExpression>(empty, "setting", std::vector<index_t>{49});
            case 49: return std::make_unique<ParameterExpression>(0, "key", 50);
            case 50: return std::make_unique<ValueExpression>([](VariableScope&) -> value_t { return EnumValue{RSK_SHUFFLE_BOSS_SOULS, "RSK_SHUFFLE_BOSS_SOULS"}; }, "RSK_SHUFFLE_BOSS_SOULS", false);
            case 51: return std::make_unique<ValueExpression>([](VariableScope&) -> value_t { return EnumValue{RO_BOSS_SOULS_ON_PLUS_GANON, "RO_BOSS_SOULS_ON_PLUS_GANON"}; }, "RO_BOSS_SOULS_ON_PLUS_GANON", false);
            case 52: return std::make_unique<ExternFunctionCallExpression>(empty, "has", std::vector<index_t>{53});
            case 53: return std::make_unique<ParameterExpression>(0, "item", 45);
            case 54: return std::make_unique<MatchArm>(55, 56, false);
            case 55: return std::make_unique<MatchArmPatterns>(std::vector<index_t>{});
            case 56: return std::make_unique<ValueExpression>([](VariableScope&) -> value_t { return false; }, "never", false);

            default: throw std::out_of_range("Invalid tracker expression index.");
        }
    };
    // clang-format on

    std::unique_ptr<LogicExpression> e = createExpression(index);
    e->SetResolver(GetExampleExpressions);
    auto ptr = e.get();
    cache.emplace(index, std::move(e));
    return ptr;
}

static ExpressionTable::ExpressionRow CreateUserGuideRow(index_t index, std::optional<value_t> childDay,
                                                         std::optional<value_t> childNight,
                                                         std::optional<value_t> adultDay,
                                                         std::optional<value_t> adultNight,
                                                         std::vector<ExpressionTable::ExpressionRow> children = {}) {
    ExpressionTable::ExpressionRow row;
    row.Expression = GetExampleExpressions(index);
    row.ChildDay = std::move(childDay);
    row.ChildNight = std::move(childNight);
    row.AdultDay = std::move(adultDay);
    row.AdultNight = std::move(adultNight);
    row.Children = std::move(children);
    row.ChildrenInitialized = true;
    return row;
}

static ExpressionTable::ExpressionRow CreateUserGuideConditionRow(index_t index, ExpressionTable::ExpressionRow body) {
    auto row = CreateUserGuideRow(index, body.ChildDay, body.ChildNight, body.AdultDay, body.AdultNight);
    row.ConditionBodies.emplace_back(std::move(body));
    return row;
}

static LogicTrackerNode CreateUserGuideNode(size_t id, const char* name, ExpressionTable::ExpressionRow root,
                                            bool expandExpression = false,
                                            RandomizerRegion sourceRegion = RR_KAKARIKO_VILLAGE,
                                            const char* sourceName = "Kakariko Village") {
    LogicTrackerNode node;
    node.NodeId = id;
    node.NodeName = name;
    node.IsExample = true;

    LogicTrackerNode::Connection connection;
    connection.ParentName = "Region: " + std::string(sourceName);
    connection.ParentRandomizerRegion = sourceRegion;
    connection.ChildDayAccess = true;
    connection.ChildNightAccess = true;
    connection.AdultDayAccess = true;
    connection.AdultNightAccess = true;

    root.Expanded = expandExpression;
    if (expandExpression) {
        for (auto& child : root.Children) {
            child.Expanded = true;
        }
    }
    connection.ExpressionTable.Root = std::move(root);
    auto [combineAll, combineChild, combineAdult] = CalculateCombines(connection.ExpressionTable.Root);
    connection.ExpressionTable.CombineAll = combineAll;
    connection.ExpressionTable.CombineChild = combineChild;
    connection.ExpressionTable.CombineAdult = combineAdult;

    node.Connections.emplace_back(std::move(connection));
    return node;
}

void LogicTrackerUserGuideWindow::DrawElement() {
    ImGui::TextWrapped("Randomizer Checks and Regions often have several ways to access them, "
                       "so the logic tracker starts with the target you're investigating (Check, Region, or Event) "
                       "and shows the logic from every connected source region.");

    const ImVec4 accentColor = ImGui::GetStyleColorVec4(ImGuiCol_CheckMark);
    const auto drawHeading = [&](const char* heading) {
        ImGui::Spacing();
        const bool open = ImGui::CollapsingHeader(heading, ImGuiTreeNodeFlags_SpanAvailWidth);
        return open;
    };
    const auto drawBullet = [&](const char* keyword, const char* description) {
        ImGui::Bullet();
        ImGui::PushFont(OTRGlobals::Instance->fontMono);
        ImGui::TextColored(accentColor, "%s", keyword);
        ImGui::PopFont();
        ImGui::SameLine();
        ImGui::TextWrapped("%s", description);
    };

    if (drawHeading("1. Read the connection: From " ICON_FA_ARROW_RIGHT " To")) {
        drawBullet("To", "The target: a check to obtain, a region to enter, or an event to trigger.");
        drawBullet("From", "The source region. Expand this row to inspect its requirements for the target.");
        drawBullet("Multiple From rows", "Different connections to the same target.");
    }

    if (drawHeading("2. Expand the requirements")) {
        drawBullet("Level 0",
                   "The full requirement expression. Click a numbered Level button to expand or collapse its parts.");
        drawBullet("Sub-expressions", "Smaller parts of a requirement, not additional connections.");
        drawBullet("true / false", "The requirement is satisfied / not satisfied.");
        drawBullet("Blank", "No evaluated value, not automatically false. The source may be unreachable for that "
                            "age/time, or the expression branch may have been skipped.");
        drawBullet("and / or", "Both requirements / either requirement.");
        drawBullet("Expression colors",
                   "Distinguish parts of a requirement; they help show the terms of the operation / function.");
    }

    if (drawHeading("3. Access " ICON_FA_ARROW_RIGHT " Availability and Age / Time")) {
        drawBullet("Access", "Left of the arrow: when you can reach the From region.");
        drawBullet("Available", "Right of the arrow: when this connection's requirements are satisfied.");
        drawBullet("All / Child / Adult", "Columns combine matching results across ages or times.");
    }

    if (drawHeading("4. Follow dependencies")) {
        drawBullet("Check gear", "Investigate how to reach the check.");
        drawBullet("Region gear", "Investigate how to reach the From region.");
        drawBullet("Event gear", "Investigate where and how an event can be triggered. Events are logical states "
                                 "or capabilities, not necessarily item checks.");
        drawBullet("Originally to", "A shuffled entrance's original destination. To shows its current destination.");
        drawBullet("Undiscovered Entrance", "Hides the entrance name to prevent spoilers. It can be shown using the "
                                            "console command: acpue (Available Checks - Process Undiscovered Exits)");
    }

    if (drawHeading("5. any_age")) {
        drawBullet("any_age(condition)",
                   "Tests a requirement using the ages that can reach the current From region. "
                   "Child and Adult flags can both be enabled; at least one age must have access.");
        drawBullet("Shared actions", "An action possible as a reachable Adult can satisfy a shared requirement "
                                     "for Child. This does not mean Child can equip Adult-only items.");
        drawBullet("Age/time columns", "Still name the outer connection's context. Inside this helper, a row "
                                       "such as is_adult() can be true in a Child column because the helper is "
                                       "testing access with different age flags.");
    }

    if (drawHeading("6. Match expressions")) {
        drawBullet("match value", "Selects a requirement based on a value, such as an item, setting, or distance. "
                                  "The compact <N arms> display shows how many branches exist; expand its Level "
                                  "button to inspect the input and arms.");
        drawBullet("Pattern: body", "The pattern before the colon is compared with the input. The body after "
                                    "the colon is the requirement used when that arm matches. Arms are checked "
                                    "in order; normally the first matching arm supplies the result, even if false.");
        drawBullet("A or B:", "Multiple patterns before the colon match either input value. This is different "
                              "from an or inside the requirement body.");
        drawBullet("_:", "An optional catch-all pattern, placed last. If no arm matches and there is no "
                         "catch-all, the result is false for a boolean match or zero for a numeric match.");
        drawBullet("Trailing or", "An or at the end of an arm enables fallthrough: if its body is false, try "
                                  "the next arm's body without requiring its pattern to match. Continue until "
                                  "a body is true or an arm without trailing or ends the chain.");
        drawBullet("Blank bodies", "Unmatched bodies and later skipped arms have no evaluated value. A visited "
                                   "arm can show false for its pattern test while its body remains blank; "
                                   "this does not mean the body itself failed.");
    }

    if (drawHeading("7. Examples")) {
        ImGui::Indent(25.0f);
        ImGui::TextWrapped("For these examples, assume each From region is reachable at every age and time. "
                           "Expand headers and Level buttons as in the tracker; gear navigation is disabled here.");

        // clang-format off
        static auto examples = [] {
            const value_t storms = EnumValue{RG_SONG_OF_STORMS, "RG_SONG_OF_STORMS"};
            const value_t scale = EnumValue{RG_SILVER_SCALE, "RG_SILVER_SCALE"};
            const value_t ironBoots = EnumValue{RG_IRON_BOOTS, "RG_IRON_BOOTS"};
            const value_t powerBracelet = EnumValue{RG_POWER_BRACELET, "RG_POWER_BRACELET"};
            const value_t hoverBoots = EnumValue{RG_HOVER_BOOTS, "RG_HOVER_BOOTS"};
            const value_t hammer = EnumValue{RG_MEGATON_HAMMER, "RG_MEGATON_HAMMER"};
            const value_t ganonSoul = EnumValue{RG_GANON_SOUL, "RG_GANON_SOUL"};
            const value_t gohmaSoul = EnumValue{RG_GOHMA_SOUL, "RG_GOHMA_SOUL"};
            const value_t dodongoSoul = EnumValue{RG_KING_DODONGO_SOUL, "RG_KING_DODONGO_SOUL"};
            const value_t barinadeSoul = EnumValue{RG_BARINADE_SOUL, "RG_BARINADE_SOUL"};
            const value_t phantomGanonSoul = EnumValue{RG_PHANTOM_GANON_SOUL, "RG_PHANTOM_GANON_SOUL"};
            const value_t volvagiaSoul = EnumValue{RG_VOLVAGIA_SOUL, "RG_VOLVAGIA_SOUL"};
            const value_t morphaSoul = EnumValue{RG_MORPHA_SOUL, "RG_MORPHA_SOUL"};
            const value_t bongoSoul = EnumValue{RG_BONGO_BONGO_SOUL, "RG_BONGO_BONGO_SOUL"};
            const value_t twinrovaSoul = EnumValue{RG_TWINROVA_SOUL, "RG_TWINROVA_SOUL"};
            const value_t bossSoulSetting = EnumValue{RSK_SHUFFLE_BOSS_SOULS, "RSK_SHUFFLE_BOSS_SOULS"};
            const value_t bossesOnly = EnumValue{RO_BOSS_SOULS_ON, "RO_BOSS_SOULS_ON"};
            const value_t includingGanon = EnumValue{RO_BOSS_SOULS_ON_PLUS_GANON, "RO_BOSS_SOULS_ON_PLUS_GANON"};
            return std::to_array({
                CreateUserGuideNode(0, "Check: ZR Frogs in the Rain",
                    CreateUserGuideRow(0, true, true, false, false, {
                        CreateUserGuideRow(1, true, true, false, false),
                        CreateUserGuideRow(2, true, true, std::nullopt, std::nullopt, {
                            CreateUserGuideRow(3, storms, storms, std::nullopt, std::nullopt)
                        })
                    }), false, RR_ZORAS_RIVER, "Zora River"),
                CreateUserGuideNode(1, "Region: ZR From Shortcut",
                    CreateUserGuideRow(5, false, false, true, true, {
                        CreateUserGuideRow(6, false, false, false, false, {
                            CreateUserGuideRow(7, scale, scale, scale, scale)
                        }),
                        CreateUserGuideRow(9, false, false, true, true, {
                            CreateUserGuideRow(10, ironBoots, ironBoots, ironBoots, ironBoots)
                        })
                    }), false, RR_ZORAS_RIVER, "Zora River"),
                CreateUserGuideNode(2, "Event: LOGIC_BUG_ACCESS",
                    CreateUserGuideRow(12, false, false, false, false, {
                        CreateUserGuideRow(13, powerBracelet, powerBracelet, powerBracelet, powerBracelet)
                    }), false, RR_DESERT_COLOSSUS, "Desert Colossus"),
                CreateUserGuideNode(3, "Region: ZR Pillar",
                    CreateUserGuideRow(15, true, true, true, true, {
                        CreateUserGuideRow(16, true, true, false, false, {
                            CreateUserGuideRow(1, true, true, false, false),
                            CreateUserGuideRow(12, true, true, std::nullopt, std::nullopt, {
                                CreateUserGuideRow(13, powerBracelet, powerBracelet, std::nullopt, std::nullopt)
                            })
                        }),
                        CreateUserGuideRow(17, std::nullopt, std::nullopt, true, true, {
                            CreateUserGuideRow(18, std::nullopt, std::nullopt, hoverBoots, hoverBoots)
                        })
                    }), true, RR_ZR_ATOP_LADDER, "ZR Atop Ladder"),
                CreateUserGuideNode(4, "Region: ZR Fairy Grotto",
                    CreateUserGuideRow(20, true, true, true, true, {
                        CreateUserGuideConditionRow(21, CreateUserGuideRow(23, true, true, true, true, {
                            CreateUserGuideRow(24, true, true, true, true, {
                                CreateUserGuideRow(25, false, false, false, false),
                                CreateUserGuideRow(26, true, true, true, true, {
                                    CreateUserGuideRow(27, hammer, hammer, hammer, hammer)
                                })
                            })
                        }))
                    }), true, RR_ZR_ATOP_LADDER, "ZR Atop Ladder"),
                CreateUserGuideNode(5, "Sub-expression: Ganondorf Hint / has_boss_soul match",
                    CreateUserGuideRow(29, true, true, true, true, {
                        CreateUserGuideRow(30, ganonSoul, ganonSoul, ganonSoul, ganonSoul),
                        CreateUserGuideRow(31, false, false, false, false, {
                            CreateUserGuideRow(32, false, false, false, false, {
                                CreateUserGuideRow(33, gohmaSoul, gohmaSoul, gohmaSoul, gohmaSoul),
                                CreateUserGuideRow(34, dodongoSoul, dodongoSoul, dodongoSoul, dodongoSoul),
                                CreateUserGuideRow(35, barinadeSoul, barinadeSoul, barinadeSoul, barinadeSoul),
                                CreateUserGuideRow(36, phantomGanonSoul, phantomGanonSoul, phantomGanonSoul, phantomGanonSoul),
                                CreateUserGuideRow(37, volvagiaSoul, volvagiaSoul, volvagiaSoul, volvagiaSoul),
                                CreateUserGuideRow(38, morphaSoul, morphaSoul, morphaSoul, morphaSoul),
                                CreateUserGuideRow(39, bongoSoul, bongoSoul, bongoSoul, bongoSoul),
                                CreateUserGuideRow(40, twinrovaSoul, twinrovaSoul, twinrovaSoul, twinrovaSoul)
                            }),
                            CreateUserGuideRow(41, std::nullopt, std::nullopt, std::nullopt, std::nullopt, {
                                CreateUserGuideRow(42, std::nullopt, std::nullopt, std::nullopt, std::nullopt)
                            })
                        }),
                        CreateUserGuideRow(43, true, true, true, true, {
                            CreateUserGuideRow(44, true, true, true, true, {
                                CreateUserGuideRow(45, ganonSoul, ganonSoul, ganonSoul, ganonSoul)
                            }),
                            CreateUserGuideRow(46, true, true, true, true, {
                                CreateUserGuideRow(47, true, true, true, true, {
                                    CreateUserGuideRow(48, bossesOnly, bossesOnly, bossesOnly, bossesOnly, {
                                        CreateUserGuideRow(49, bossSoulSetting, bossSoulSetting, bossSoulSetting, bossSoulSetting)
                                    }),
                                    CreateUserGuideRow(51, includingGanon, includingGanon, includingGanon, includingGanon)
                                }),
                                CreateUserGuideRow(52, std::nullopt, std::nullopt, std::nullopt, std::nullopt, {
                                    CreateUserGuideRow(53, std::nullopt, std::nullopt, std::nullopt, std::nullopt)
                                })
                            })
                        }),
                        CreateUserGuideRow(54, std::nullopt, std::nullopt, std::nullopt, std::nullopt, {
                            CreateUserGuideRow(55, std::nullopt, std::nullopt, std::nullopt, std::nullopt),
                            CreateUserGuideRow(56, std::nullopt, std::nullopt, std::nullopt, std::nullopt)
                        })
                    }), true, RR_GANONS_TOWER_GANONDORF_LAIR, "Ganondorf's Lair"),
            });
        }();
        // clang-format on

        const auto drawExample = [&](const char* title, size_t index, const char* explanation) {
            if (drawHeading(title)) {
                ImGui::Indent(25.0f);
                ImGui::TextWrapped("%s", explanation);
                DrawNode(examples[index]);
                ImGui::Unindent(25.0f);
            }
        };

        drawExample(
            "Check example", 0,
            "Assume Song of Storms can be played. The check requires Child and can_use(RG_SONG_OF_STORMS), "
            "so All access becomes Child availability. Adult fails the first AND term, leaving the song test blank.");
        drawExample(
            "Entrance example", 1,
            "Assume Silver Scale is missing and Iron Boots are usable as Adult. The scale OR boots alternatives "
            "give Adult availability.");
        drawExample("Event example", 2,
                    "Assume Power Bracelet is missing. This region's has(RG_POWER_BRACELET) requirement is false for "
                    "every age/time.");
        drawExample("Sub-expression example", 3,
                    "Assume Power Bracelet is owned and Hover Boots are usable as Adult. Child passes the nested "
                    "is_child() AND bracelet branch. Adult skips the bracelet test but passes the boots branch.");
        drawExample("any_age example", 4,
                    "The entrance requires any_age(blast_or_smash()). The helper tests has_explosives() "
                    "OR can_use(RG_MEGATON_HAMMER). Assume both ages reach the ladder, explosives are "
                    "unavailable, and Adult can use the Hammer, but Child cannot. any_age describe how "
                    "Adult can break the boulder and give Child access.");
        drawExample("Match expression example", 5,
                    "This example shows the complete match inside has_boss_soul(item), not "
                    "the helper's preceding shuffle-disabled alternative. The input is RG_GANON_SOUL and "
                    "boss souls are shuffled without Ganon (RO_BOSS_SOULS_ON). None of the first arm's eight "
                    "patterns match, so its has(item) body is blank. The Ganon arm matches; the setting "
                    "comparison is true, skipping has(RG_GANON_SOUL) and the _: never catch-all. Results "
                    "combine into All. These arms have no trailing-or fallthrough.");
        ImGui::Unindent(25.0f);
    }
}

void LogicTrackerUserGuideWindow::InitElement() {
}

void LogicTrackerUserGuideWindow::UpdateElement() {
}
