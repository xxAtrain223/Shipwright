#include "randomizer_logic_tracker.h"

#include "location_access.h"
#include "entrance.h"
#include "../../OTRGlobals.h"
#include "logic_expression.h"

#include <deque>
#include <stack>
#include <ranges>

#include "logic/generated/soh/functions.gen.h"

// clang-format off
static value_t has_adapter(VariableScope& scope) {
    auto item = static_cast<RandomizerGet>(ToInt<uint32_t>(scope.get("item")));
    return static_cast<value_t>(has(item));
}

static value_t check_price_adapter(VariableScope& scope) {
    return static_cast<value_t>(check_price());
}

std::span<LogicExpression* const> GetExpressions() {
    static auto expressions = std::to_array<LogicExpression*>({
    /* 0*/ new AndExpression({1, 3}),
    /* 1*/ new ExternFunctionCallExpression(has_adapter, "has", {"item"}, {2}),
    /* 2*/ new ValueExpression([](VariableScope& scope){ return static_cast<value_t>(RG_SPEAK_KOKIRI); }, "RG_SPEAK_KOKIRI", false),
    /* 3*/ new ComparisonExpression("<=", 4, 5),
    /* 4*/ new ExternFunctionCallExpression(check_price_adapter, "check_price", {}, {}),
    /* 5*/ new FunctionCallExpression("wallet_capacity", {}, {}, 6),
    /* 6*/ new TernaryExpression(7, 9, 10),
    /* 7*/ new ExternFunctionCallExpression(has_adapter, "has",  {"item"}, {8}),
    /* 8*/ new ValueExpression([](VariableScope& scope){ return static_cast<value_t>(RG_TYCOON_WALLET); }, "RG_TYCOON_WALLET", false),
    /* 9*/ new ValueExpression([](VariableScope& scope){ return static_cast<value_t>(999); }, "999", false),
    /*10*/ new TernaryExpression(11, 13, 14),
    /*11*/ new ExternFunctionCallExpression(has_adapter, "has",  {"item"}, {12}),
    /*12*/ new ValueExpression([](VariableScope& scope){ return static_cast<value_t>(RG_GIANT_WALLET); }, "RG_GIANT_WALLET", false),
    /*13*/ new ValueExpression([](VariableScope& scope){ return static_cast<value_t>(500); }, "500", false),
    /*14*/ new TernaryExpression(15, 17, 18),
    /*15*/ new ExternFunctionCallExpression(has_adapter, "has",  {"item"}, {16}),
    /*16*/ new ValueExpression([](VariableScope& scope){ return static_cast<value_t>(RG_ADULT_WALLET); }, "RG_ADULT_WALLET", false),
    /*17*/ new ValueExpression([](VariableScope& scope){ return static_cast<value_t>(200); }, "200", false),
    /*18*/ new TernaryExpression(19, 21, 22),
    /*19*/ new ExternFunctionCallExpression(has_adapter, "has",  {"item"}, {20}),
    /*20*/ new ValueExpression([](VariableScope& scope){ return static_cast<value_t>(RG_CHILD_WALLET); }, "RG_CHILD_WALLET", false),
    /*21*/ new ValueExpression([](VariableScope& scope){ return static_cast<value_t>(99); }, "99", false),
    /*22*/ new ValueExpression([](VariableScope& scope){ return static_cast<value_t>(0); }, "0", false),
    });

    return { expressions.data(), expressions.size() };
}
// clang-format on

static std::string ToString(const value_t& value) {
    return std::visit(
        [](auto v) -> std::string {
            using V = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<V, bool>) {
                return v ? "true" : "false";
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
        LogicExpression* Expression;
        std::vector<ExpressionRow> Children;
        std::optional<value_t> ChildDay;
        std::optional<value_t> ChildNight;
        std::optional<value_t> AdultDay;
        std::optional<value_t> AdultNight;
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

class LogicStateGuard {
  public:
    LogicStateGuard()
        : isChild(logic->IsChild), isAdult(logic->IsAdult), atDay(logic->AtDay), atNight(logic->AtNight),
          currentRegionKey(logic->CurrentRegionKey) {
    }

    ~LogicStateGuard() {
        logic->IsChild = isChild;
        logic->IsAdult = isAdult;
        logic->AtDay = atDay;
        logic->AtNight = atNight;
        logic->CurrentRegionKey = currentRegionKey;
    }

  private:
    bool isChild;
    bool isAdult;
    bool atDay;
    bool atNight;
    RandomizerRegion currentRegionKey;
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

    return access;
}

std::unordered_map<std::string, std::vector<RandomizerEventInfo>> randomizerEventMap;

bool expandingNode = false;
size_t expandNodeId = -1;
size_t clearNodesAfterNodeID = -1;
std::vector<LogicTrackerNode> nodes;

static ExpressionTable::ExpressionRow CreateExpressionRows(LogicExpression* expression) {
    ExpressionTable::ExpressionRow row;
    row.Expression = expression;
    row.Expanded = false;

    const auto& children = expression->GetChildren();
    row.Children.reserve(children.size());
    for (const auto& child : children) {
        row.Children.emplace_back(CreateExpressionRows(child));
    }

    return row;
}

enum class AgeTime { ChildDay, ChildNight, AdultDay, AdultNight };

static void PopulateExpressionValues(ExpressionTable::ExpressionRow& row, const EvalResult& eval,
                                     AgeTime ageTime) {
    if (ageTime == AgeTime::ChildDay) {
        row.ChildDay = eval.Result;
    } else if (ageTime == AgeTime::ChildNight) {
        row.ChildNight = eval.Result;
    } else if (ageTime == AgeTime::AdultDay) {
        row.AdultDay = eval.Result;
    } else if (ageTime == AgeTime::AdultNight) {
        row.AdultNight = eval.Result;
    }

    for (auto& rowChild : row.Children) {
        for (const auto& evalChild : eval.Children) {
            if (rowChild.Expression == evalChild.Expression) {
                PopulateExpressionValues(rowChild, evalChild, ageTime);
                break;
            }
        }
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
    return { combineAll, combineChild, combineAdult };
}

static void PopulateConnectionExpression(LogicTrackerNode::Connection& connection, const std::string& expressionStr) {
    auto expression = GetExpressions()[0];

    LogicStateGuard logicStateGuard;

    try {
        logic->IsChild = false;
        logic->IsAdult = false;
        logic->AtDay = false;
        logic->AtNight = false;

        connection.ExpressionTable.Root = CreateExpressionRows(expression);

        if (connection.ChildDayAccess) {
            logic->IsChild = true;
            logic->AtDay = true;

            VariableScope scope;
            const auto& eval = connection.ExpressionTable.Root.Expression->Evaluate(scope);
            PopulateExpressionValues(connection.ExpressionTable.Root, eval, AgeTime::ChildDay);

            logic->IsChild = false;
            logic->AtDay = false;
        }
        if (connection.ChildNightAccess) {
            logic->IsChild = true;
            logic->AtNight = true;

            VariableScope scope;
            const auto& eval = connection.ExpressionTable.Root.Expression->Evaluate(scope);
            PopulateExpressionValues(connection.ExpressionTable.Root, eval, AgeTime::ChildNight);

            logic->IsChild = false;
            logic->AtNight = false;
        }
        if (connection.AdultDayAccess) {
            logic->IsAdult = true;
            logic->AtDay = true;

            VariableScope scope;
            const auto& eval = connection.ExpressionTable.Root.Expression->Evaluate(scope);
            PopulateExpressionValues(connection.ExpressionTable.Root, eval, AgeTime::AdultDay);

            logic->IsAdult = false;
            logic->AtDay = false;
        }
        if (connection.AdultNightAccess) {
            logic->IsAdult = true;
            logic->AtNight = true;

            VariableScope scope;
            const auto& eval = connection.ExpressionTable.Root.Expression->Evaluate(scope);
            PopulateExpressionValues(connection.ExpressionTable.Root, eval, AgeTime::AdultNight);

            logic->IsAdult = false;
            logic->AtNight = false;
        }
    } catch (const std::exception& e) {
        connection.ExpressionTable.Root.ErrorMessage = std::string("Eval Error: ") + e.what();
        connection.ExpressionTable.Root.Children.clear();
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

                PopulateConnectionExpression(connection, locationAccess.GetConditionStr());

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

        PopulateConnectionExpression(connection, entrance->GetConditionStr());

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

        PopulateConnectionExpression(connection, eventInfo.ConditionStr);

        node.Connections.emplace_back(std::move(connection));
        logic->CurrentRegionKey = RR_NONE;
    }

    nodes.emplace_back(std::move(node));

    expandNodeId = node.NodeId;

    showEvent.clear();
}

static void DrawExpressionRow(const LogicTrackerNode& node, const ExpressionTable& table,
                              ExpressionTable::ExpressionRow& row, int level) {
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

    const auto& expression = *row.Expression;
    if (!row.Children.empty()) {
        if (ImGui::Button(std::to_string(level).c_str())) {
            row.Expanded = !row.Expanded;
        }
    } else {
        ImGui::BeginDisabled();
        ImGui::Button(std::to_string(level).c_str());
        ImGui::EndDisabled();

        // TODO: re-implement
        /*if (expression.GetType() == LogicExpression::Type::Value &&
            expression.GetValueType() == LogicExpression::ValueType::Enum) {
            std::string identifier = expression.ToString();
            auto it = randomizerEventMap.find(identifier);
            if (it != randomizerEventMap.end()) {
                ImGui::SameLine();
                if (ImGui::Button(ICON_FA_COGS)) {
                    LogicTrackerWindow::ShowRandomizerEvent(identifier);
                    clearNodesAfterNodeID = node.NodeId;
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Show Event Logic");
                }
            }
        }*/
    }

    ImGui::TableNextColumn();
    expression.Draw();
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

    if (row.Expanded) {
        for (auto& child : row.Children) {
            DrawExpressionRow(node, table, child, level + 1);
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

    if (expandingNode) {
        ImGui::SetNextItemOpen(expandNodeId == node.NodeId, ImGuiCond_Always);
    }

    bool nodeOpen = ImGui::CollapsingHeader(("To " + node.NodeName).c_str(),
                                            ImGuiTreeNodeFlags_AllowOverlap | ImGuiTreeNodeFlags_SpanFullWidth);
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

            if (expandingNode && node.Connections.size() == 1) {
                ImGui::SetNextItemOpen(expandNodeId == node.NodeId, ImGuiCond_Always);
            }

            bool connectionOpen =
                ImGui::CollapsingHeader(("From " + connection.ParentName).c_str(),
                                        ImGuiTreeNodeFlags_AllowOverlap | ImGuiTreeNodeFlags_SpanAvailWidth);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Show Connection Logic");
            }

            std::string accessAvailable =
                GetAccessString(connection) + " " + ICON_FA_ARROW_RIGHT + " " + GetAvailableString(connection);

            ImGui::SameLine(ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(accessAvailable.c_str()).x);
            ImGui::TextUnformatted(accessAvailable.c_str());
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Connection Access " ICON_FA_ARROW_RIGHT " Available");
            }

            ImGui::SameLine();
            if (ImGui::Button(ICON_FA_COGS)) {
                LogicTrackerWindow::ShowRandomizerRegion(connection.ParentRandomizerRegion, node.RandomizerRegion);
                clearNodesAfterNodeID = node.NodeId;
            }
            if (ImGui::IsItemHovered()) {
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
    for (auto* expression : GetExpressions()) {
        expression->ResolveChildren();
    }
}

void LogicTrackerWindow::UpdateElement() {
    PopulateRandomizerEventMap();
    CalculateShowRandomizerCheck();
    CalculateShowRandomizerRegion();
    CalculateShowRandomizerEvent();
}