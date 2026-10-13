#include "logic_expression.h"

#include "imgui.h"

#include <array>
#include <optional>
#include <sstream>
#include <stdexcept>

static auto fontColors = std::to_array({
    ImVec4(0.5f, 0.5f, 1.0f, 1.0f), // Blue
    ImVec4(0.5f, 1.0f, 0.5f, 1.0f), // Green
    ImVec4(1.0f, 0.5f, 0.5f, 1.0f), // Red
    ImVec4(1.0f, 1.0f, 0.5f, 1.0f), // Yellow
    ImVec4(1.0f, 0.5f, 1.0f, 1.0f), // Magenta
    ImVec4(0.5f, 1.0f, 1.0f, 1.0f), // Cyan
});

static bool NeedsParentheses(const LogicExpression* parent, const LogicExpression* child) {
    if (dynamic_cast<const FunctionCallExpression*>(child)) {
        return false;
    }

    const bool childIsBinary = dynamic_cast<const BinaryExpression*>(child) != nullptr;
    const bool childIsComparison = dynamic_cast<const ComparisonExpression*>(child) != nullptr;
    const bool childIsTernary = dynamic_cast<const TernaryExpression*>(child) != nullptr;

    if (dynamic_cast<const NotExpression*>(parent)) {
        return childIsBinary || childIsComparison || childIsTernary;
    }

    if (dynamic_cast<const ComparisonExpression*>(parent)) {
        return childIsBinary || childIsComparison || childIsTernary;
    }

    if (dynamic_cast<const ArithmeticExpression*>(parent)) {
        return childIsBinary || childIsComparison || childIsTernary ||
               dynamic_cast<const NotExpression*>(child) != nullptr;
    }

    if (dynamic_cast<const BinaryExpression*>(parent)) {
        return childIsTernary ||
               (dynamic_cast<const AndExpression*>(parent) && dynamic_cast<const OrExpression*>(child)) ||
               (dynamic_cast<const OrExpression*>(parent) && dynamic_cast<const AndExpression*>(child));
    }

    if (dynamic_cast<const TernaryExpression*>(parent)) {
        return childIsBinary || childIsComparison || childIsTernary;
    }

    return false;
}

static std::string WrapParentheses(const LogicExpression* parent, const LogicExpression* child) {
    return NeedsParentheses(parent, child) ? "(" + child->ToString() + ")" : child->ToString();
}

void LogicExpression::Draw() const {
    ImGui::TextWrapped("%s", ToString().c_str());
}

void LogicExpression::ResolveChildren() {
    if (_childrenResolved) {
        return;
    }

    auto expressions = GetExpressions();
    for (auto& child : _children) {
        const auto index = child.index;
        assert(index < expressions.size());
        child.expression = expressions[index];
    }

    _childrenResolved = true;
}

void VariableScope::push(std::string_view variableName, value_t value) {
    auto [it, inserted] = variables.try_emplace(variableName);
    try {
        it->second.push(value);
    } catch (...) {
        if (inserted) {
            variables.erase(it);
        }
        throw;
    }
}

value_t VariableScope::get(std::string_view variableName) const {
    return variables.at(variableName).top();
}

void VariableScope::pop(std::string_view variableName) {
    auto& stack = variables.at(variableName);
    stack.pop();
    if (stack.empty()) {
        variables.erase(variableName);
    }
}

VariableScope::~VariableScope() {
    assert(variables.empty());
}

namespace {
class ScopedParameterBindings {
  public:
    ScopedParameterBindings(VariableScope& scope, std::span<const std::string_view> names)
        : _scope(scope), _names(names) {
    }

    ScopedParameterBindings(const ScopedParameterBindings&) = delete;
    ScopedParameterBindings& operator=(const ScopedParameterBindings&) = delete;

    void Push(value_t value) {
        assert(_boundCount < _names.size());
        _scope.push(_names[_boundCount], value);
        ++_boundCount;
    }

    ~ScopedParameterBindings() noexcept {
        while (_boundCount > 0) {
            _scope.pop(_names[--_boundCount]);
        }
    }

  private:
    VariableScope& _scope;
    std::span<const std::string_view> _names;
    size_t _boundCount = 0;
};
} // namespace

static void DrawColoredWrappedText(const std::vector<std::pair<ImVec4, std::string>>& segments) {
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    ImVec2 pos = ImGui::GetCursorScreenPos();
    float wrapWidth = ImGui::GetContentRegionAvail().x;
    float x = pos.x;
    float y = pos.y;
    float startX = x;
    ImFont* font = ImGui::GetFont();
    float fontSize = ImGui::GetFontSize();

    for (const auto& [color, text] : segments) {
        std::istringstream iss(text + " ");
        std::string word;
        bool firstWordInSegment = true;
        while (std::getline(iss, word, ' ')) {
            std::string toDraw = word;
            if (!firstWordInSegment) {
                toDraw = " " + word;
            }
            ImVec2 textSize = ImGui::CalcTextSize(toDraw.c_str());
            if (x + textSize.x > startX + wrapWidth && x > startX) {
                x = startX;
                y += fontSize;
                toDraw = word;
                textSize = ImGui::CalcTextSize(toDraw.c_str());
            }
            draw_list->AddText(font, fontSize, ImVec2(x, y), ImGui::ColorConvertFloat4ToU32(color), toDraw.c_str());
            x += textSize.x;
            firstWordInSegment = false;
        }
    }
    ImGui::SetCursorScreenPos(ImVec2(pos.x, y + fontSize));
}

BinaryExpression::BinaryExpression(std::vector<index_t> children) {
    _children.insert(_children.end(), children.begin(), children.end());
}

std::string BinaryExpression::ToString() const {
    std::string str;
    auto children = GetChildren();
    for (size_t i = 0; i < children.size(); i++) {
        str += WrapParentheses(this, children[i]);
        if (i < children.size() - 1) {
            str += " " + GetOperation() + " ";
        }
    }
    return str;
}

void BinaryExpression::Draw() const {
    std::vector<std::pair<ImVec4, std::string>> segments;
    auto children = GetChildren();
    for (size_t i = 0; i < children.size(); i++) {
        segments.emplace_back(fontColors[i % fontColors.size()], WrapParentheses(this, children[i]));
        if (i < children.size() - 1) {
            segments.emplace_back(ImGui::GetStyleColorVec4(ImGuiCol_Text), " " + GetOperation() + " ");
        }
    }
    DrawColoredWrappedText(segments);
}

EvalResult BinaryExpression::Evaluate(VariableScope& scope) const {
    auto children = GetChildren();
    assert(children.size() >= 2);

    EvalResult evalResult{};
    evalResult.Expression = this;
    evalResult.Children.push_back(children[0]->Evaluate(scope));

    value_t value = evalResult.Children.back().Result;

    for (size_t i = 1; i < children.size(); ++i) {
        value_t shortCircuitResult;
        if (TryShortCircuit(value, shortCircuitResult)) {
            value = shortCircuitResult;
            break;
        }

        evalResult.Children.push_back(children[i]->Evaluate(scope));
        value = Combine(value, evalResult.Children.back().Result);
    }

    evalResult.Result = value;
    return evalResult;
}

bool BinaryExpression::TryShortCircuit(const value_t&, value_t&) const {
    return false;
}

AndExpression::AndExpression(std::vector<index_t> children) : BinaryExpression(children) {
}

bool AndExpression::TryShortCircuit(const value_t& left, value_t& result) const {
    if (!ToBool(left)) {
        result = false;
        return true;
    }
    return false;
}

value_t AndExpression::Combine(const value_t& left, const value_t& right) const {
    return ToBool(left) && ToBool(right);
}

std::string AndExpression::GetOperation() const {
    return "and";
}

OrExpression::OrExpression(std::vector<index_t> children) : BinaryExpression(children) {
}

bool OrExpression::TryShortCircuit(const value_t& left, value_t& result) const {
    if (ToBool(left)) {
        result = true;
        return true;
    }
    return false;
}

value_t OrExpression::Combine(const value_t& left, const value_t& right) const {
    return ToBool(left) || ToBool(right);
}

std::string OrExpression::GetOperation() const {
    return "or";
}

NotExpression::NotExpression(index_t childIndex) {
    _children = { childIndex };
}

EvalResult NotExpression::Evaluate(VariableScope& scope) const {
    EvalResult evalResult;
    evalResult.Expression = this;

    auto child = GetChildren()[0];
    auto childResult = child->Evaluate(scope);
    evalResult.Children.push_back(childResult);
    evalResult.Result = !ToBool(childResult.Result);

    return evalResult;
}

std::string NotExpression::ToString() const {
    return "not " + WrapParentheses(this, GetChildren()[0]);
}

void NotExpression::Draw() const {
    DrawColoredWrappedText(
        { { ImGui::GetStyleColorVec4(ImGuiCol_Text), "not " }, { fontColors[0], WrapParentheses(this, GetChildren()[0]) } });
}

ArithmeticExpression::ArithmeticExpression(std::string_view operation, std::vector<index_t> children)
    : BinaryExpression(children) {
    _operation = operation;
}

bool ArithmeticExpression::TryShortCircuit(const value_t& left, value_t& result) const {
    return false;
}

value_t ArithmeticExpression::Combine(const value_t& left, const value_t& right) const {
    auto operate = [&](auto a, auto b) -> value_t {
        using ArithmeticType = decltype(+a + +b);

        const auto l = static_cast<ArithmeticType>(a);
        const auto r = static_cast<ArithmeticType>(b);

        if (_operation == "+")
            return l + r;
        if (_operation == "-")
            return l - r;
        if (_operation == "*")
            return l * r;
        if (_operation == "/") {
            if (r == 0) {
                throw std::runtime_error("Division by zero in arithmetic expression");
            }
            return l / r;
        }

        throw std::runtime_error("Unknown arithmetic operation: " + std::string(_operation));
    };

    return std::visit(operate, left, right);
}

std::string ArithmeticExpression::GetOperation() const {
    return std::string(_operation);
}

ComparisonExpression::ComparisonExpression(std::string_view operation, index_t left, index_t right) {
    _operation = operation;
    _children = { left, right };
}

EvalResult ComparisonExpression::Evaluate(VariableScope& scope) const {
    EvalResult evalResult;
    evalResult.Expression = this;

    auto children = GetChildren();

    auto left = children[0];
    auto leftResult = left->Evaluate(scope);
    evalResult.Children.push_back(leftResult);

    auto right = children[1];
    auto rightResult = right->Evaluate(scope);
    evalResult.Children.push_back(rightResult);

    auto compare = [&](auto a, auto b) -> bool {
        using ComparisonType = decltype(+a + +b);

        const auto l = static_cast<ComparisonType>(a);
        const auto r = static_cast<ComparisonType>(b);

        if (_operation == "==")
            return l == r;
        if (_operation == "!=")
            return l != r;
        if (_operation == "<")
            return l < r;
        if (_operation == "<=")
            return l <= r;
        if (_operation == ">")
            return l > r;
        if (_operation == ">=")
            return l >= r;

        throw std::runtime_error("Unknown comparison operator: " + std::string(_operation));
    };

    evalResult.Result = std::visit(compare, leftResult.Result, rightResult.Result);

    return evalResult;
}

std::string ComparisonExpression::ToString() const {
    auto children = GetChildren();
    return WrapParentheses(this, children[0]) + " " + std::string(_operation) + " " +
           WrapParentheses(this, children[1]);
}

void ComparisonExpression::Draw() const {
    auto children = GetChildren();
    DrawColoredWrappedText({
        { fontColors[0], WrapParentheses(this, children[0]) },
        { ImGui::GetStyleColorVec4(ImGuiCol_Text), " " + std::string(_operation) + " " },
        { fontColors[1], WrapParentheses(this, children[1]) },
    });
}

FunctionCallExpression::FunctionCallExpression(std::string_view functionName,
                                               std::vector<std::string_view> parameterNames,
                       std::vector<index_t> parameters) {
    _functionName = functionName;
    _parameterNames = parameterNames;
    _children.insert(_children.end(), parameters.begin(), parameters.end());
}

FunctionCallExpression::FunctionCallExpression(std::string_view functionName,
                                               std::vector<std::string_view> parameterNames,
                        std::vector<index_t> parameters, index_t definition)
    : FunctionCallExpression(functionName, parameterNames, parameters) {
    _children.push_back(definition);
}

EvalResult FunctionCallExpression::Evaluate(VariableScope& scope) const {
    EvalResult evalResult;
    evalResult.Expression = this;

    auto children = GetChildren();
    std::vector<value_t> parameterValues(_parameterNames.size());

    for (size_t i = 0; i < _parameterNames.size(); i++) {
        auto childResult = children[i]->Evaluate(scope);
        parameterValues[i] = childResult.Result;
        evalResult.Children.push_back(childResult);
    }

    ScopedParameterBindings bindings(scope, _parameterNames);
    for (const auto& value : parameterValues) {
        bindings.Push(value);
    }

    auto functionResult = children.back()->Evaluate(scope);
    evalResult.Children.push_back(functionResult);
    evalResult.Result = functionResult.Result;

    return evalResult;
}

std::string FunctionCallExpression::ToString() const {
    auto children = GetChildren();
    std::string str = std::string(_functionName) + "(";
    for (size_t i = 0; i < _parameterNames.size(); i++) {
        str += std::string(_parameterNames[i]) + ": " + children[i]->ToString();
        if (i < _parameterNames.size() - 1) {
            str += ", ";
        }
    }
    str += ")";
    return str;
}

void FunctionCallExpression::Draw() const {
    auto children = GetChildren();
    std::vector<std::pair<ImVec4, std::string>> segments;
    segments.emplace_back(fontColors[0], _functionName);
    segments.emplace_back(ImGui::GetStyleColorVec4(ImGuiCol_Text), "(");
    for (size_t i = 0; i < _parameterNames.size(); i++) {
        segments.emplace_back(ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled),
                                std::string(_parameterNames[i]) + ": ");
        segments.emplace_back(fontColors[(i + 1) % fontColors.size()], children[i]->ToString());
        if (i < _parameterNames.size() - 1) {
            segments.emplace_back(ImGui::GetStyleColorVec4(ImGuiCol_Text), ", ");
        }
    }
    segments.emplace_back(ImGui::GetStyleColorVec4(ImGuiCol_Text), ")");
    DrawColoredWrappedText(segments);
}

ExternFunctionCallExpression::ExternFunctionCallExpression(function_t function, std::string_view functionName,
                                 std::vector<std::string_view> parameterNames, std::vector<index_t> parameters)
    : FunctionCallExpression(functionName, parameterNames, parameters) {
    _function = function;
}

EvalResult ExternFunctionCallExpression::Evaluate(VariableScope& scope) const {
    EvalResult evalResult;
    evalResult.Expression = this;

    auto children = GetChildren();
    std::vector<value_t> parameterValues(_parameterNames.size());

    for (size_t i = 0; i < _parameterNames.size(); i++) {
        auto childResult = children[i]->Evaluate(scope);
        parameterValues[i] = childResult.Result;
        evalResult.Children.push_back(childResult);
    }

    ScopedParameterBindings bindings(scope, _parameterNames);
    for (const auto& value : parameterValues) {
        bindings.Push(value);
    }

    evalResult.Result = _function(scope);

    return evalResult;
}

ValueExpression::ValueExpression(function_t function, std::string_view expression, bool isEvent) {
    _function = function;
    _expression = expression;
    _isEvent = isEvent;
}

EvalResult ValueExpression::Evaluate(VariableScope& scope) const {
    EvalResult evalResult;
    evalResult.Expression = this;
    evalResult.Result = _function(scope);
    return evalResult;
}

std::string ValueExpression::ToString() const {
    return std::string(_expression);
}

void ValueExpression::Draw() const {
    DrawColoredWrappedText({ { fontColors[0], std::string(_expression) } });
}

TernaryExpression::TernaryExpression(index_t condition, index_t trueBranch, index_t falseBranch) {
    _children = { condition, trueBranch, falseBranch };
}

EvalResult TernaryExpression::Evaluate(VariableScope& scope) const {
    EvalResult evalResult;
    evalResult.Expression = this;

    auto children = GetChildren();

    auto conditionResult = children[0]->Evaluate(scope);
    evalResult.Children.push_back(conditionResult);

    auto branchResult =
        ToBool(conditionResult.Result) ? children[1]->Evaluate(scope) : children[2]->Evaluate(scope);
    evalResult.Children.push_back(branchResult);
    evalResult.Result = branchResult.Result;

    return evalResult;
}

std::string TernaryExpression::ToString() const {
    auto children = GetChildren();
    return WrapParentheses(this, children[0]) + " ? " + WrapParentheses(this, children[1]) + " : " +
           WrapParentheses(this, children[2]);
}

void TernaryExpression::Draw() const {
    auto children = GetChildren();
    DrawColoredWrappedText({
        { fontColors[0], WrapParentheses(this, children[0]) },
        { ImGui::GetStyleColorVec4(ImGuiCol_Text), " ? " },
        { fontColors[1], WrapParentheses(this, children[1]) },
        { ImGui::GetStyleColorVec4(ImGuiCol_Text), " : " },
        { fontColors[2], WrapParentheses(this, children[2]) }
    });
}