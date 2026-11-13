#include "logic_expression.h"

#include "imgui.h"

#include <array>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <utility>

static const ImVec4 expressionTextColor(1.0f, 1.0f, 1.0f, 1.0f);
static const ImVec4 expressionDisabledTextColor(0.5f, 0.5f, 0.5f, 1.0f);

static auto fontColors = std::to_array({
    ImVec4(138.0f / 255.0f, 173.0f / 255.0f, 224.0f / 255.0f, 1.0f), // Blue
    ImVec4(145.0f / 255.0f, 191.0f / 255.0f, 137.0f / 255.0f, 1.0f), // Sage green
    ImVec4(220.0f / 255.0f, 145.0f / 255.0f, 133.0f / 255.0f, 1.0f), // Coral
    ImVec4(222.0f / 255.0f, 196.0f / 255.0f, 119.0f / 255.0f, 1.0f), // Gold
    ImVec4(179.0f / 255.0f, 157.0f / 255.0f, 219.0f / 255.0f, 1.0f), // Cool violet
});

static bool NeedsParentheses(const LogicExpression* parent, const LogicExpression* child) {
    child = child->GetDisplayExpression();
    if (dynamic_cast<const FunctionCallExpression*>(child)) {
        return false;
    }

    const bool childIsBinary = dynamic_cast<const BinaryExpression*>(child) != nullptr;
    const bool childIsComparison = dynamic_cast<const ComparisonExpression*>(child) != nullptr;
    const bool childIsTernary = dynamic_cast<const TernaryExpression*>(child) != nullptr;

    if (dynamic_cast<const InvokeExpression*>(parent)) {
        return childIsBinary || childIsComparison || childIsTernary ||
               dynamic_cast<const NotExpression*>(child) != nullptr ||
               dynamic_cast<const ConditionExpression*>(child) != nullptr;
    }

    if (dynamic_cast<const NotExpression*>(parent) || dynamic_cast<const ComparisonExpression*>(parent) ||
        dynamic_cast<const TernaryExpression*>(parent)) {
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

    return false;
}

static std::string WrapParentheses(const LogicExpression* parent, const LogicExpression* child) {
    return NeedsParentheses(parent, child) ? "(" + child->ToString() + ")" : child->ToString();
}

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
        const ImU32 packedColor = ImGui::ColorConvertFloat4ToU32(color);
        std::string_view remaining(text);
        while (!remaining.empty()) {
            const size_t lineEnd = remaining.find('\n');
            const std::string_view line = remaining.substr(0, lineEnd);
            for (size_t wordStart = 0;;) {
                const size_t space = line.find(' ', wordStart);
                const size_t wordEnd = space == std::string_view::npos ? line.size() : space;
                const std::string_view word = line.substr(wordStart, wordEnd - wordStart);
                const char* textBegin = word.data() - (wordStart > 0 ? 1 : 0);
                const char* textEnd = word.data() + word.substr(0, word.find('\0')).size();
                ImVec2 textSize = ImGui::CalcTextSize(textBegin, textEnd);
                if (x + textSize.x > startX + wrapWidth && x > startX) {
                    x = startX;
                    y += fontSize;
                    textBegin = word.data();
                    textSize = ImGui::CalcTextSize(textBegin, textEnd);
                }
                draw_list->AddText(font, fontSize, ImVec2(x, y), packedColor, textBegin, textEnd);
                x += textSize.x;
                if (space == std::string_view::npos) {
                    break;
                }
                wordStart = space + 1;
            }
            if (lineEnd == std::string_view::npos) {
                break;
            }
            x = startX;
            y += fontSize;
            remaining.remove_prefix(lineEnd + 1);
        }
    }
    ImGui::SetCursorScreenPos(ImVec2(pos.x, y + fontSize));
}

void LogicExpression::Draw(size_t startColorIndex) const {
    auto drawingSegments = GetDrawingSegments(startColorIndex);
    DrawSegments(drawingSegments);
}

void LogicExpression::DrawSegments(const std::vector<std::pair<ImVec4, std::string>>& segments) {
    DrawColoredWrappedText(segments);
}

void LogicExpression::SetResolver(expression_resolver_t resolver) {
    _resolver = resolver;
}

std::vector<std::pair<ImVec4, std::string>> LogicExpression::GetDrawingSegments(size_t startColorIndex) const {
    std::vector<std::pair<ImVec4, std::string>> segments;
    segments.reserve(1);
    segments.emplace_back(fontColors[startColorIndex % fontColors.size()], ToString());
    return segments;
}

size_t LogicExpression::GetChildColorIndex(size_t childIndex, size_t startColorIndex) const {
    return (startColorIndex % fontColors.size() + childIndex % fontColors.size()) % fontColors.size();
}

const LogicExpression* LogicExpression::GetDisplayExpression() const {
    return this;
}

LogicExpressionCache::LogicExpressionCache(uint64_t expressionCount, expression_factory_t factory)
    : _expressionCount(expressionCount), _factory(factory) {
    if (expressionCount > uint64_t{ std::numeric_limits<index_t>::max() } + 1 || !factory) {
        throw std::invalid_argument("Invalid logic expression factory or count");
    }
}

LogicExpression* LogicExpressionCache::GetExpression(index_t index) {
    if (index >= _expressionCount) {
        throw std::out_of_range("Logic expression index out of range: " + std::to_string(index));
    }
    auto [it, inserted] = _expressions.try_emplace(index);
    if (!inserted) {
        if (!it->second) {
            throw std::logic_error("Recursive logic expression construction: " + std::to_string(index));
        }
        return it->second.get();
    }
    try {
        auto expression = _factory(index);
        if (!expression) {
            throw std::runtime_error("Logic expression factory returned null: " + std::to_string(index));
        }
        auto* result = expression.get();
        _expressions.at(index) = std::move(expression);
        return result;
    } catch (...) {
        _expressions.erase(index);
        throw;
    }
}

uint64_t LogicExpressionCache::GetExpressionCount() const {
    return _expressionCount;
}

size_t LogicExpressionCache::GetCachedExpressionCount() const {
    size_t count = 0;
    for (const auto& [index, expression] : _expressions) {
        if (expression) {
            ++count;
        }
    }
    return count;
}

size_t LogicRootKeyHash::operator()(const LogicRootKey& key) const {
    size_t hash = std::hash<LogicRootKind>{}(key.Kind);
    const auto combine = [&](size_t value) { hash ^= value + size_t{ 0x9e3779b9 } + (hash << 6) + (hash >> 2); };
    combine(std::hash<std::string_view>{}(key.OwnerRegion));
    combine(std::hash<std::string_view>{}(key.Entry));
    combine(std::hash<uint32_t>{}(key.Occurrence));
    return hash;
}

LogicRootRegistry::LogicRootRegistry(LogicRootMap roots, uint64_t expressionCount, expression_resolver_t resolver)
    : _roots(std::move(roots)), _resolver(resolver) {
    if (expressionCount > uint64_t{ std::numeric_limits<index_t>::max() } + 1 || !resolver) {
        throw std::invalid_argument("Invalid logic expression resolver or count");
    }
    for (const auto& [key, index] : _roots) {
        switch (key.Kind) {
            case LogicRootKind::Location:
            case LogicRootKind::Event:
            case LogicRootKind::Exit:
                break;
            default:
                throw std::invalid_argument("Unknown logic root kind");
        }
        if (key.OwnerRegion.empty() || key.Entry.empty()) {
            throw std::invalid_argument("Logic root owner and entry must not be empty");
        }
        if (index >= expressionCount) {
            throw std::invalid_argument("Invalid logic root expression: " + std::string(key.OwnerRegion) + "/" +
                                        std::string(key.Entry));
        }
    }
}

std::optional<index_t> LogicRootRegistry::Find(const LogicRootKey& key) const {
    const auto root = _roots.find(key);
    if (root != _roots.end()) {
        return root->second;
    }
    return std::nullopt;
}

const LogicExpression* LogicRootRegistry::GetExpression(const LogicRootKey& key) const {
    const auto root = Find(key);
    if (!root) {
        return nullptr;
    }
    const auto* expression = _resolver(*root);
    if (!expression) {
        throw std::runtime_error("Logic expression resolver returned null");
    }
    return expression;
}

template <LogicTarget T> ConversionExpression<T>::ConversionExpression(index_t operand) {
    _children = { operand };
}

template <LogicTarget T> EvalResult ConversionExpression<T>::Evaluate(VariableScope& scope) const {
    auto result = GetChildren()[0]->Evaluate(scope);
    result.Result = ConvertValue<T>(result.Result);
    return result;
}

template <LogicTarget T> std::string ConversionExpression<T>::ToString() const {
    return GetChildren()[0]->ToString();
}

template <LogicTarget T>
std::vector<std::pair<ImVec4, std::string>> ConversionExpression<T>::GetDrawingSegments(size_t startColorIndex) const {
    return GetChildren()[0]->GetDrawingSegments(startColorIndex);
}

template <LogicTarget T> size_t ConversionExpression<T>::GetChildColorIndex(size_t, size_t startColorIndex) const {
    return startColorIndex;
}

template <LogicTarget T> const LogicExpression* ConversionExpression<T>::GetDisplayExpression() const {
    return GetChildren()[0]->GetDisplayExpression();
}

template class ConversionExpression<bool>;
template class ConversionExpression<int8_t>;
template class ConversionExpression<int16_t>;
template class ConversionExpression<int32_t>;
template class ConversionExpression<uint8_t>;
template class ConversionExpression<uint16_t>;
template class ConversionExpression<uint32_t>;

EvalResult LogicExpression::EvaluateChild(VariableScope& scope, size_t childIndex) const {
    auto result = GetChildren()[childIndex]->Evaluate(scope);
    result.ChildIndex = childIndex;
    return result;
}

void VariableScope::push(std::string_view variableName, value_t value) {
    auto [it, inserted] = variables.try_emplace(std::string(variableName));
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
    return variables.at(std::string(variableName)).top();
}

void VariableScope::pop(std::string_view variableName) {
    const std::string name(variableName);
    auto& stack = variables.at(name);
    stack.pop();
    if (stack.empty()) {
        variables.erase(name);
    }
}

bindings_t VariableScope::Capture() const {
    bindings_t bindings;
    for (const auto& [name, values] : variables) {
        bindings.emplace(name, values.top());
    }
    return bindings;
}

std::function<bool()> MakeConditionCallback(EvalResult& argument) {
    return [&argument] {
        auto result = Invoke(argument.Result);
        const bool value = std::get<bool>(result.Result);
        argument.Invocation = std::make_shared<const EvalResult>(std::move(result));
        return value;
    };
}

VariableScope::~VariableScope() {
    assert(variables.empty());
}

namespace {
class ScopedParameterBindings {
  public:
    ScopedParameterBindings(VariableScope& scope, std::span<const std::string_view> names,
                            std::span<const EvalResult> arguments)
        : ScopedParameterBindings(scope, names.size()) {
        assert(names.size() == arguments.size());
        for (size_t i = 0; i < names.size(); ++i) {
            Push(names[i], arguments[i].Result);
        }
    }

    ScopedParameterBindings(VariableScope& scope, const bindings_t& captured)
        : ScopedParameterBindings(scope, captured.size()) {
        for (const auto& [name, value] : captured) {
            Push(name, value);
        }
    }

    ScopedParameterBindings(const ScopedParameterBindings&) = delete;
    ScopedParameterBindings& operator=(const ScopedParameterBindings&) = delete;

    ~ScopedParameterBindings() noexcept {
        while (!_names.empty()) {
            _scope.pop(_names.back());
            _names.pop_back();
        }
    }

  private:
    ScopedParameterBindings(VariableScope& scope, size_t count) : _scope(scope) {
        _names.reserve(count);
    }

    void Push(std::string_view name, const value_t& value) {
        _scope.push(name, value);
        _names.push_back(name);
    }

    VariableScope& _scope;
    std::vector<std::string_view> _names;
};

} // namespace

EvalResult Invoke(const value_t& value) {
    const auto* condition = std::get_if<condition_t>(&value);
    if (!condition || !*condition) {
        throw std::runtime_error("Cannot invoke a non-condition value");
    }

    VariableScope invocationScope;
    ScopedParameterBindings bindings(invocationScope, (*condition)->Bindings);

    auto result = (*condition)->Body->Evaluate(invocationScope);
    result.Result = ToBool(result.Result);
    return result;
}

ConditionExpression::ConditionExpression(index_t body) {
    _children = { body };
}

EvalResult ConditionExpression::Evaluate(VariableScope& scope) const {
    EvalResult result;
    result.Expression = this;
    result.Result = std::make_shared<const ConditionValue>(ConditionValue{ GetChildren()[0], scope.Capture() });
    return result;
}

std::string ConditionExpression::ToString() const {
    return GetChildren()[0]->ToString();
}

std::vector<std::pair<ImVec4, std::string>> ConditionExpression::GetDrawingSegments(size_t startColorIndex) const {
    return GetChildren()[0]->GetDrawingSegments(GetChildColorIndex(0, startColorIndex));
}

InvokeExpression::InvokeExpression(index_t callee) {
    _children = { callee };
}

EvalResult InvokeExpression::Evaluate(VariableScope& scope) const {
    EvalResult result;
    result.Expression = this;
    result.Children.push_back(EvaluateChild(scope, 0));
    result.Invocation = std::make_shared<const EvalResult>(Invoke(result.Children.back().Result));
    result.Result = result.Invocation->Result;
    return result;
}

std::string InvokeExpression::ToString() const {
    return WrapParentheses(this, GetChildren()[0]) + "()";
}

std::vector<std::pair<ImVec4, std::string>> InvokeExpression::GetDrawingSegments(size_t startColorIndex) const {
    std::vector<std::pair<ImVec4, std::string>> segments;
    segments.reserve(2);
    segments.emplace_back(fontColors[GetChildColorIndex(0, startColorIndex)], WrapParentheses(this, GetChildren()[0]));
    segments.emplace_back(expressionTextColor, "()");
    return segments;
}

BinaryExpression::BinaryExpression(std::vector<index_t> children) {
    _children.insert(_children.end(), children.begin(), children.end());
}

std::string BinaryExpression::ToString() const {
    std::string str;
    auto children = GetChildren();
    const std::string separator = children.size() > 1 ? " " + GetOperation() + " " : "";
    for (size_t i = 0; i < children.size(); i++) {
        str += WrapParentheses(this, children[i]);
        if (i < children.size() - 1) {
            str += separator;
        }
    }
    return str;
}

std::vector<std::pair<ImVec4, std::string>> BinaryExpression::GetDrawingSegments(size_t startColorIndex) const {
    std::vector<std::pair<ImVec4, std::string>> segments;
    auto children = GetChildren();
    segments.reserve(children.empty() ? 0 : 2 * children.size() - 1);
    const std::string separator = children.size() > 1 ? " " + GetOperation() + " " : "";
    for (size_t i = 0; i < children.size(); i++) {
        segments.emplace_back(fontColors[GetChildColorIndex(i, startColorIndex)], WrapParentheses(this, children[i]));
        if (i < children.size() - 1) {
            segments.emplace_back(expressionTextColor, separator);
        }
    }
    return segments;
}

EvalResult BinaryExpression::Evaluate(VariableScope& scope) const {
    auto children = GetChildren();
    assert(children.size() >= 2);

    EvalResult evalResult{};
    evalResult.Expression = this;
    evalResult.Children.push_back(EvaluateChild(scope, 0));

    value_t value = evalResult.Children.back().Result;

    for (size_t i = 1; i < children.size(); ++i) {
        value_t shortCircuitResult;
        if (TryShortCircuit(value, shortCircuitResult)) {
            value = shortCircuitResult;
            break;
        }

        evalResult.Children.push_back(EvaluateChild(scope, i));
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

    auto childResult = EvaluateChild(scope, 0);
    evalResult.Children.push_back(childResult);
    evalResult.Result = !ToBool(childResult.Result);

    return evalResult;
}

std::string NotExpression::ToString() const {
    return "not " + WrapParentheses(this, GetChildren()[0]);
}

std::vector<std::pair<ImVec4, std::string>> NotExpression::GetDrawingSegments(size_t startColorIndex) const {
    std::vector<std::pair<ImVec4, std::string>> segments;
    segments.reserve(2);
    segments.emplace_back(expressionTextColor, "not ");
    segments.emplace_back(fontColors[GetChildColorIndex(0, startColorIndex)], WrapParentheses(this, GetChildren()[0]));
    return segments;
}

ArithmeticExpression::ArithmeticExpression(std::string_view operation, std::vector<index_t> children)
    : BinaryExpression(children) {
    _operation = operation;
}

bool ArithmeticExpression::TryShortCircuit(const value_t& left, value_t& result) const {
    return false;
}

template <typename T> static auto UnwrapEnumValue(T value) {
    if constexpr (std::is_same_v<T, EnumValue>) {
        return value.Value;
    } else {
        return value;
    }
}

value_t ArithmeticExpression::Combine(const value_t& left, const value_t& right) const {
    auto operate = [&](auto leftValue, auto rightValue) -> value_t {
        const auto a = UnwrapEnumValue(leftValue);
        const auto b = UnwrapEnumValue(rightValue);
        if constexpr (std::is_integral_v<decltype(a)> && std::is_integral_v<decltype(b)>) {
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
        } else {
            throw std::runtime_error("Cannot use a condition in arithmetic");
        }
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

    auto leftResult = EvaluateChild(scope, 0);
    evalResult.Children.push_back(leftResult);

    auto rightResult = EvaluateChild(scope, 1);
    evalResult.Children.push_back(rightResult);

    auto compare = [&](auto leftValue, auto rightValue) -> bool {
        const auto a = UnwrapEnumValue(leftValue);
        const auto b = UnwrapEnumValue(rightValue);
        if constexpr (std::is_integral_v<decltype(a)> && std::is_integral_v<decltype(b)>) {
            using ComparisonType = decltype(+a + +b);

            const auto l = static_cast<ComparisonType>(a);
            const auto r = static_cast<ComparisonType>(b);

            if (_operation == "==" || _operation == "is")
                return l == r;
            if (_operation == "!=" || _operation == "is not")
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
        } else {
            throw std::runtime_error("Cannot compare condition values");
        }
    };

    evalResult.Result = std::visit(compare, leftResult.Result, rightResult.Result);

    return evalResult;
}

std::string ComparisonExpression::ToString() const {
    auto children = GetChildren();
    return WrapParentheses(this, children[0]) + " " + std::string(_operation) + " " +
           WrapParentheses(this, children[1]);
}

std::vector<std::pair<ImVec4, std::string>> ComparisonExpression::GetDrawingSegments(size_t startColorIndex) const {
    auto children = GetChildren();
    std::vector<std::pair<ImVec4, std::string>> segments;
    segments.reserve(3);
    segments.emplace_back(fontColors[GetChildColorIndex(0, startColorIndex)], WrapParentheses(this, children[0]));
    segments.emplace_back(expressionTextColor, " " + std::string(_operation) + " ");
    segments.emplace_back(fontColors[GetChildColorIndex(1, startColorIndex)], WrapParentheses(this, children[1]));
    return segments;
}

FunctionCallExpression::FunctionCallExpression(std::string_view functionName, std::vector<index_t> parameters) {
    _functionName = functionName;
    _children.insert(_children.end(), parameters.begin(), parameters.end());
    _parameterCount = parameters.size();
}

FunctionCallExpression::FunctionCallExpression(std::string_view functionName, std::vector<index_t> parameters,
                                               index_t definition)
    : FunctionCallExpression(functionName, parameters) {
    _children.push_back(definition);
}

EvalResult FunctionCallExpression::EvaluateArguments(VariableScope& scope) const {
    EvalResult evalResult;
    evalResult.Expression = this;
    for (size_t i = 0; i < _parameterCount; i++) {
        evalResult.Children.push_back(EvaluateChild(scope, i));
    }
    return evalResult;
}

EvalResult FunctionCallExpression::Evaluate(VariableScope& scope) const {
    auto evalResult = EvaluateArguments(scope);
    auto children = GetChildren();

    std::vector<std::string_view> parameterNames(_parameterCount);
    for (size_t i = 0; i < _parameterCount; i++) {
        auto parameter = dynamic_cast<ParameterExpression*>(children[i]);
        parameterNames[i] = parameter->_parameterName;
    }
    ScopedParameterBindings bindings(scope, parameterNames, evalResult.Children);

    auto functionResult = EvaluateChild(scope, _parameterCount);
    evalResult.Children.push_back(functionResult);
    evalResult.Result = functionResult.Result;

    return evalResult;
}

std::string FunctionCallExpression::ToString() const {
    auto children = GetChildren();
    std::string str;
    str.reserve(_functionName.size() + 2);
    str += _functionName;
    str += '(';
    for (size_t i = 0; i < _parameterCount; i++) {
        str += children[i]->ToString();
        if (i < _parameterCount - 1) {
            str += ", ";
        }
    }
    str += ")";
    return str;
}

static ImVec4 ScaleRGB(ImVec4 color, float scale) {
    auto scaled = color;
    scaled.x *= scale;
    scaled.y *= scale;
    scaled.z *= scale;
    return scaled;
}

size_t FunctionCallExpression::GetChildColorIndex(size_t childIndex, size_t startColorIndex) const {
    return LogicExpression::GetChildColorIndex(childIndex < _parameterCount ? childIndex + 1 : 0, startColorIndex);
}

std::vector<std::pair<ImVec4, std::string>> FunctionCallExpression::GetDrawingSegments(size_t startColorIndex) const {
    auto children = GetChildren();
    std::vector<std::pair<ImVec4, std::string>> segments;
    segments.reserve(_parameterCount == 0 ? 3 : 3 * _parameterCount + 2);
    segments.emplace_back(fontColors[startColorIndex % fontColors.size()], _functionName);
    segments.emplace_back(expressionTextColor, "(");
    for (size_t i = 0; i < _parameterCount; i++) {
        auto parameter = dynamic_cast<ParameterExpression*>(children[i]);
        const auto colorIndex = GetChildColorIndex(i, startColorIndex);
        /*segments.emplace_back(ScaleRGB(fontColors[colorIndex], 0.5f),
                              std::string(parameter->_parameterName) + ": ");*/
        segments.emplace_back(expressionDisabledTextColor, std::string(parameter->_parameterName) + ": ");
        segments.emplace_back(fontColors[colorIndex], parameter->GetChildren()[0]->ToString());
        if (i < _parameterCount - 1) {
            segments.emplace_back(expressionTextColor, ", ");
        }
    }
    segments.emplace_back(expressionTextColor, ")");
    return segments;
}

ExternFunctionCallExpression::ExternFunctionCallExpression(extern_function_t function, std::string_view functionName,
                                                           std::vector<index_t> parameters)
    : FunctionCallExpression(functionName, parameters) {
    _function = function;
}

EvalResult ExternFunctionCallExpression::Evaluate(VariableScope& scope) const {
    auto evalResult = EvaluateArguments(scope);
    evalResult.Result = _function(evalResult.Children);

    return evalResult;
}

ParameterExpression::ParameterExpression(uint8_t parameterIndex, std::string_view parameterName, index_t child)
    : _parameterIndex(parameterIndex), _parameterName(parameterName) {
    _children = { child };
}

EvalResult ParameterExpression::Evaluate(VariableScope& scope) const {
    EvalResult result;
    result.Expression = this;
    result.Children.push_back(EvaluateChild(scope, 0));
    result.Result = result.Children[0].Result;
    return result;
}

std::string ParameterExpression::ToString() const {
    return std::string(_parameterName) + ": " + GetChildren()[0]->ToString();
}

std::vector<std::pair<ImVec4, std::string>> ParameterExpression::GetDrawingSegments(size_t startColorIndex) const {
    auto childSegments = GetChildren()[0]->GetDrawingSegments(GetChildColorIndex(0, startColorIndex));
    std::vector<std::pair<ImVec4, std::string>> segments;
    segments.reserve(1 + childSegments.size());
    segments.emplace_back(expressionDisabledTextColor, std::string(_parameterName) + ": ");
    segments.insert(segments.end(), std::make_move_iterator(childSegments.begin()),
                    std::make_move_iterator(childSegments.end()));
    return segments;
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

std::vector<std::pair<ImVec4, std::string>> ValueExpression::GetDrawingSegments(size_t startColorIndex) const {
    std::vector<std::pair<ImVec4, std::string>> segments;
    segments.reserve(1);
    segments.emplace_back(fontColors[startColorIndex % fontColors.size()], _expression);
    return segments;
}

TernaryExpression::TernaryExpression(index_t condition, index_t trueBranch, index_t falseBranch) {
    _children = { condition, trueBranch, falseBranch };
}

EvalResult TernaryExpression::Evaluate(VariableScope& scope) const {
    EvalResult evalResult;
    evalResult.Expression = this;

    auto conditionResult = EvaluateChild(scope, 0);
    evalResult.Children.push_back(conditionResult);

    auto branchResult = EvaluateChild(scope, ToBool(conditionResult.Result) ? 1 : 2);
    evalResult.Children.push_back(branchResult);
    evalResult.Result = branchResult.Result;

    return evalResult;
}

std::string TernaryExpression::ToString() const {
    auto children = GetChildren();
    std::string result = WrapParentheses(this, children[0]);
    const std::string trueBranch = WrapParentheses(this, children[1]);
    const std::string falseBranch = WrapParentheses(this, children[2]);
    result.reserve(result.size() + trueBranch.size() + falseBranch.size() + 6);
    result += " ? ";
    result += trueBranch;
    result += " : ";
    result += falseBranch;
    return result;
}

std::vector<std::pair<ImVec4, std::string>> TernaryExpression::GetDrawingSegments(size_t startColorIndex) const {
    auto children = GetChildren();
    std::vector<std::pair<ImVec4, std::string>> segments;
    segments.reserve(5);
    segments.emplace_back(fontColors[GetChildColorIndex(0, startColorIndex)], WrapParentheses(this, children[0]));
    segments.emplace_back(expressionTextColor, " ? ");
    segments.emplace_back(fontColors[GetChildColorIndex(1, startColorIndex)], WrapParentheses(this, children[1]));
    segments.emplace_back(expressionTextColor, " : ");
    segments.emplace_back(fontColors[GetChildColorIndex(2, startColorIndex)], WrapParentheses(this, children[2]));
    return segments;
}

static constexpr std::string_view matchDiscriminantName = "$match";

template <typename T>
requires(LogicTarget<T> || std::is_same_v<T, value_t>) MatchExpression<T>::MatchExpression(index_t discriminant,
                                                                                           std::vector<index_t> arms) {
    _children.push_back(discriminant);
    _children.insert(_children.end(), arms.begin(), arms.end());
}

template <typename T>
requires(LogicTarget<T> || std::is_same_v<T, value_t>) EvalResult MatchExpression<T>::Evaluate(VariableScope& scope)
const {
    EvalResult evalResult;
    evalResult.Expression = this;
    evalResult.Result = ConvertResult(false);
    evalResult.Children.push_back(EvaluateChild(scope, 0));
    const std::array names{ matchDiscriminantName };
    ScopedParameterBindings binding(scope, names, evalResult.Children);

    auto children = GetChildren();
    bool active = false;
    for (size_t i = 1; i < children.size(); ++i) {
        auto arm = dynamic_cast<const MatchArm*>(children[i]);
        assert(arm != nullptr);
        auto armResult = active ? arm->EvaluateBody(scope) : arm->Evaluate(scope);
        armResult.ChildIndex = i;
        const bool matched = active || armResult.Children.size() == 2;
        evalResult.Children.push_back(armResult);
        if (matched) {
            evalResult.Result = ConvertResult(armResult.Result);
            if (!arm->_fallThrough || i + 1 == children.size() || ToBool(evalResult.Result)) {
                break;
            }
            active = true;
        }
    }
    return evalResult;
}

#if 0 // Omit match arms to cut down on the displayed text
template <typename T>
requires(LogicTarget<T> || std::is_same_v<T, value_t>) std::string MatchExpression<T>::ToString()
const {
    auto children = GetChildren();
    std::string result = "match " + children[0]->ToString() + " {";
    for (size_t i = 1; i < children.size(); ++i) {
        result += "\n" + children[i]->ToString();
    }
    return result + "\n}";
}

template <typename T>
requires(LogicTarget<T> || std::is_same_v<T, value_t>)
    std::vector<std::pair<ImVec4, std::string>> MatchExpression<T>::GetDrawingSegments(size_t startColorIndex)
const {
    auto children = GetChildren();
    const auto textColor = expressionTextColor;
    std::vector<std::pair<ImVec4, std::string>> segments = {
        { textColor, "match " },
        { fontColors[GetChildColorIndex(0, startColorIndex)], children[0]->ToString() },
        { textColor, " {" },
    };
    for (size_t i = 1; i < children.size(); ++i) {
        segments.push_back({ textColor, "\n" });
        segments.push_back({ fontColors[GetChildColorIndex(i, startColorIndex)], children[i]->ToString() });
    }
    segments.push_back({ textColor, "\n}" });
    return segments;
}
#else
template <typename T>
requires(LogicTarget<T> || std::is_same_v<T, value_t>) std::string MatchExpression<T>::ToString()
const {
    auto children = GetChildren();
    return "match " + children[0]->ToString() + " { <" + std::to_string(children.size() - 1) + " arms> }";
}

template <typename T>
requires(LogicTarget<T> || std::is_same_v<T, value_t>)
    std::vector<std::pair<ImVec4, std::string>> MatchExpression<T>::GetDrawingSegments(size_t startColorIndex)
const {
    auto children = GetChildren();
    std::vector<std::pair<ImVec4, std::string>> segments;
    segments.reserve(5);
    segments.emplace_back(expressionTextColor, "match ");
    segments.emplace_back(fontColors[GetChildColorIndex(0, startColorIndex)], children[0]->ToString());
    segments.emplace_back(expressionTextColor, " { ");
    segments.emplace_back(fontColors[GetChildColorIndex(1, startColorIndex)],
                          "<" + std::to_string(children.size() - 1) + " arms>");
    segments.emplace_back(expressionTextColor, " }");
    return segments;
}
#endif

template class MatchExpression<value_t>;
template class MatchExpression<bool>;
template class MatchExpression<int8_t>;
template class MatchExpression<int16_t>;
template class MatchExpression<int32_t>;
template class MatchExpression<uint8_t>;
template class MatchExpression<uint16_t>;
template class MatchExpression<uint32_t>;

MatchArm::MatchArm(index_t patterns, index_t expression, bool fallThrough) {
    _children.push_back(patterns);
    _children.push_back(expression);
    _fallThrough = fallThrough;
}

EvalResult MatchArm::Evaluate(VariableScope& scope) const {
    EvalResult evalResult;
    evalResult.Expression = this;
    evalResult.Children.push_back(EvaluateChild(scope, 0));
    if (ToBool(evalResult.Children[0].Result)) {
        evalResult.Children.push_back(EvaluateChild(scope, 1));
        evalResult.Result = evalResult.Children.back().Result;
    } else {
        evalResult.Result = false;
    }
    return evalResult;
}

EvalResult MatchArm::EvaluateBody(VariableScope& scope) const {
    EvalResult evalResult;
    evalResult.Expression = this;
    evalResult.Children.push_back(EvaluateChild(scope, 1));
    evalResult.Result = evalResult.Children[0].Result;
    return evalResult;
}

std::string MatchArm::ToString() const {
    auto children = GetChildren();
    return children[0]->ToString() + ": " + children[1]->ToString() + (_fallThrough ? " or" : "");
}

std::vector<std::pair<ImVec4, std::string>> MatchArm::GetDrawingSegments(size_t startColorIndex) const {
    auto children = GetChildren();
    const auto textColor = expressionTextColor;
    std::vector<std::pair<ImVec4, std::string>> segments;
    segments.reserve(_fallThrough ? 4 : 3);
    segments.emplace_back(fontColors[GetChildColorIndex(0, startColorIndex)], children[0]->ToString());
    segments.emplace_back(textColor, ": ");
    segments.emplace_back(fontColors[GetChildColorIndex(1, startColorIndex)], children[1]->ToString());
    if (_fallThrough) {
        segments.emplace_back(textColor, " or");
    }
    return segments;
}

MatchArmPatterns::MatchArmPatterns(std::vector<index_t> children) {
    _children.insert(_children.end(), children.begin(), children.end());
}

EvalResult MatchArmPatterns::Evaluate(VariableScope& scope) const {
    EvalResult evalResult;
    evalResult.Expression = this;
    evalResult.Result = _children.empty();
    if (_children.empty()) {
        return evalResult;
    }

    const auto discriminant = scope.get(matchDiscriminantName);
    for (size_t i = 0; i < _children.size(); ++i) {
        auto patternResult = EvaluateChild(scope, i);
        evalResult.Children.push_back(patternResult);
        const bool matched = std::visit(
            [](auto leftValue, auto rightValue) -> bool {
                const auto a = UnwrapEnumValue(leftValue);
                const auto b = UnwrapEnumValue(rightValue);
                if constexpr (std::is_integral_v<decltype(a)> && std::is_integral_v<decltype(b)>) {
                    using ComparisonType = decltype(+a + +b);
                    return static_cast<ComparisonType>(a) == static_cast<ComparisonType>(b);
                } else {
                    throw std::runtime_error("Cannot compare condition values");
                }
            },
            discriminant, patternResult.Result);
        if (matched) {
            evalResult.Result = true;
            break;
        }
    }
    return evalResult;
}

std::string MatchArmPatterns::ToString() const {
    if (_children.empty()) {
        return "_";
    }
    auto children = GetChildren();
    std::string result = children[0]->ToString();
    for (size_t i = 1; i < children.size(); ++i) {
        result += " or ";
        result += children[i]->ToString();
    }
    return result;
}

std::vector<std::pair<ImVec4, std::string>> MatchArmPatterns::GetDrawingSegments(size_t startColorIndex) const {
    std::vector<std::pair<ImVec4, std::string>> segments;
    if (_children.empty()) {
        segments.reserve(1);
        segments.emplace_back(expressionTextColor, "_");
        return segments;
    }
    auto children = GetChildren();
    segments.reserve(2 * children.size() - 1);
    for (size_t i = 0; i < children.size(); ++i) {
        if (i != 0) {
            segments.emplace_back(expressionTextColor, " or ");
        }
        segments.emplace_back(fontColors[GetChildColorIndex(i, startColorIndex)], children[i]->ToString());
    }
    return segments;
}
