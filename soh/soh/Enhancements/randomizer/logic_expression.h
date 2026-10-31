#pragma once

#include <cassert>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <stack>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <variant>
#include <vector>

struct VariableScope;
struct ConditionValue;
struct ImVec4;

using index_t = uint32_t;
using condition_t = std::shared_ptr<const ConditionValue>;
using value_t = std::variant<bool, int8_t, int16_t, int32_t, uint8_t, uint16_t, uint32_t, condition_t>;
using bindings_t = std::unordered_map<std::string, value_t>;
using function_t = value_t(*)(VariableScope&);

static bool ToBool(const std::optional<value_t>& value) {
    if (!value.has_value()) {
        return false;
    }
    return std::visit(
        [](auto v) -> bool {
            using V = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<V, bool>) {
                return v;
            } else if constexpr (std::is_integral_v<V>) {
                return v != 0;
            } else {
                throw std::bad_variant_access();
            }
        },
        value.value());
}

template <typename T> static T ToInt(const std::optional<value_t>& value) {
    if (!value.has_value()) {
        return 0;
    }
    return std::visit(
        [](auto v) -> T {
            using V = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<V, bool>) {
                return v ? 1 : 0;
            } else if constexpr (std::is_integral_v<V>) {
                return static_cast<T>(v);
            } else {
                throw std::bad_variant_access();
            }
        },
        value.value());
}

template <typename T>
concept LogicTarget = std::is_same_v<T, bool> || std::is_same_v<T, int8_t> || std::is_same_v<T, int16_t> ||
                      std::is_same_v<T, int32_t> || std::is_same_v<T, uint8_t> || std::is_same_v<T, uint16_t> ||
                      std::is_same_v<T, uint32_t>;

template <LogicTarget T> value_t ConvertValue(const value_t& value) {
    if constexpr (std::is_same_v<T, bool>) {
        return ToBool(value);
    } else {
        return ToInt<T>(value);
    }
}

struct EvalResult;
using extern_function_t = value_t (*)(std::span<EvalResult>);

class LogicExpression;
LogicExpression* GetExpression(index_t index);

class LogicExpression {
  public:

    virtual ~LogicExpression() = default;

    virtual EvalResult Evaluate(VariableScope& scope) const = 0;

    virtual std::string ToString() const = 0;

    virtual std::vector<std::pair<ImVec4, std::string>> GetDrawingSegments(size_t startColorIndex = 0) const;

    virtual size_t GetChildColorIndex(size_t childIndex, size_t startColorIndex) const;

    virtual const LogicExpression* GetDisplayExpression() const;

    void Draw(size_t startColorIndex = 0) const;

  protected:
    std::vector<index_t> _children;

    EvalResult EvaluateChild(VariableScope& scope, size_t childIndex) const;

  public:
    auto GetChildren() const {
        return _children | std::views::transform([](index_t child) { return GetExpression(child); });
    }
};

using expression_resolver_t = LogicExpression* (*)(index_t);

using expression_factory_t = std::unique_ptr<LogicExpression> (*)(index_t);

class LogicExpressionCache {
  public:
    LogicExpressionCache(uint64_t expressionCount, expression_factory_t factory);

    LogicExpressionCache(const LogicExpressionCache&) = delete;
    LogicExpressionCache& operator=(const LogicExpressionCache&) = delete;

    LogicExpression* GetExpression(index_t index);

    uint64_t GetExpressionCount() const;

    size_t GetCachedExpressionCount() const;

  private:
    uint64_t _expressionCount;
    expression_factory_t _factory;
    std::unordered_map<index_t, std::unique_ptr<LogicExpression>> _expressions;
};

struct ConditionValue {
    const LogicExpression* Body;
    bindings_t Bindings;
};

struct EvalResult {
    const LogicExpression* Expression = nullptr;
    value_t Result;
    std::vector<EvalResult> Children;
    size_t ChildIndex = 0;
    std::shared_ptr<const EvalResult> Invocation;

};

struct VariableScope {
    VariableScope() = default;

    VariableScope(const VariableScope&) = delete;
    VariableScope& operator=(const VariableScope&) = delete;

    void push(std::string_view variableName, value_t value);

    value_t get(std::string_view variableName) const;

    void pop(std::string_view variableName);

    bindings_t Capture() const;

    ~VariableScope();

  private:
    std::unordered_map<std::string, std::stack<value_t>> variables;
};

EvalResult Invoke(const value_t& condition);

std::function<bool()> MakeConditionCallback(EvalResult& argument);

enum class LogicRootKind { Location, Event, Exit };

struct LogicRootKey {
    LogicRootKind Kind;
    std::string_view OwnerRegion;
    std::string_view Entry;
    uint32_t Occurrence = 0;

    bool operator==(const LogicRootKey&) const = default;
};

struct LogicRootKeyHash {
    size_t operator()(const LogicRootKey& key) const;
};

using LogicRootMap = std::unordered_map<LogicRootKey, index_t, LogicRootKeyHash>;

// Generated string storage, resolver, and cached expressions must outlive the registry.
const LogicRootMap& GetLogicRoots();

class LogicRootRegistry {
  public:
    LogicRootRegistry(LogicRootMap roots, uint64_t expressionCount,
                      expression_resolver_t resolver = ::GetExpression);

    std::optional<index_t> Find(const LogicRootKey& key) const;

    const LogicExpression* GetExpression(const LogicRootKey& key) const;

  private:
    LogicRootMap _roots;
    expression_resolver_t _resolver;
};

template <LogicTarget T> class ConversionExpression : public LogicExpression {
  public:
    explicit ConversionExpression(index_t operand);

    EvalResult Evaluate(VariableScope& scope) const override;

    std::string ToString() const override;

    std::vector<std::pair<ImVec4, std::string>> GetDrawingSegments(size_t startColorIndex = 0) const override;

    size_t GetChildColorIndex(size_t childIndex, size_t startColorIndex) const override;

    const LogicExpression* GetDisplayExpression() const override;
};

class ConditionExpression : public LogicExpression {
  public:
    explicit ConditionExpression(index_t body);

    EvalResult Evaluate(VariableScope& scope) const override;

    std::string ToString() const override;

    std::vector<std::pair<ImVec4, std::string>> GetDrawingSegments(size_t startColorIndex = 0) const override;
};

class InvokeExpression : public LogicExpression {
  public:
    explicit InvokeExpression(index_t callee);

    EvalResult Evaluate(VariableScope& scope) const override;

    std::string ToString() const override;

    std::vector<std::pair<ImVec4, std::string>> GetDrawingSegments(size_t startColorIndex = 0) const override;
};

class BinaryExpression : public LogicExpression {
  public:
    BinaryExpression(std::vector<index_t> children);

    std::string ToString() const override;

    EvalResult Evaluate(VariableScope& scope) const override;

    std::vector<std::pair<ImVec4, std::string>> GetDrawingSegments(size_t startColorIndex = 0) const override;

  protected:
    virtual bool TryShortCircuit(const value_t&, value_t&) const;

    virtual value_t Combine(const value_t& left, const value_t& right) const = 0;

    virtual std::string GetOperation() const = 0;
};

class AndExpression : public BinaryExpression {
  public:
    AndExpression(std::vector<index_t> children);

  protected:
    bool TryShortCircuit(const value_t& left, value_t& result) const override;

    value_t Combine(const value_t& left, const value_t& right) const override;

    std::string GetOperation() const override;
};

class OrExpression : public BinaryExpression {
  public:
    OrExpression(std::vector<index_t> children);

  protected:
    bool TryShortCircuit(const value_t& left, value_t& result) const override;

    value_t Combine(const value_t& left, const value_t& right) const override;

    std::string GetOperation() const override;
};

class NotExpression : public LogicExpression {
  public:
    NotExpression(index_t childIndex);

    EvalResult Evaluate(VariableScope& scope) const override;

    std::string ToString() const override;

    std::vector<std::pair<ImVec4, std::string>> GetDrawingSegments(size_t startColorIndex = 0) const override;
};

class ArithmeticExpression : public BinaryExpression {
  public:
    ArithmeticExpression(std::string_view operation, std::vector<index_t> children);

  protected:
    bool TryShortCircuit(const value_t& left, value_t& result) const override;

    value_t Combine(const value_t& left, const value_t& right) const override;

    std::string GetOperation() const override;

  private:
    std::string_view _operation;
};

class ComparisonExpression : public LogicExpression {
  public:
    ComparisonExpression(std::string_view operation, index_t left, index_t right);

    EvalResult Evaluate(VariableScope& scope) const override;

    std::string ToString() const override;

    std::vector<std::pair<ImVec4, std::string>> GetDrawingSegments(size_t startColorIndex = 0) const override;

  private:
    std::string_view _operation;
};

class FunctionCallExpression : public LogicExpression {
  public:
    FunctionCallExpression(std::string_view functionName, std::vector<index_t> parameters, index_t definition);

    std::string_view GetFunctionName() const {
        return _functionName;
    }

    EvalResult Evaluate(VariableScope& scope) const override;

    std::string ToString() const override;

    std::vector<std::pair<ImVec4, std::string>> GetDrawingSegments(size_t startColorIndex = 0) const override;

    size_t GetChildColorIndex(size_t childIndex, size_t startColorIndex) const override;

  protected:
    FunctionCallExpression(std::string_view functionName, std::vector<index_t> parameters);

    EvalResult EvaluateArguments(VariableScope& scope) const;

    std::string_view _functionName;

    size_t _parameterCount;
};

class ExternFunctionCallExpression : public FunctionCallExpression {
  public:
    ExternFunctionCallExpression(extern_function_t function, std::string_view functionName,
                                 std::vector<index_t> parameters);

    EvalResult Evaluate(VariableScope& scope) const override;

  private:
    extern_function_t _function;
};

class ParameterExpression : public LogicExpression {
  public:
    ParameterExpression(uint8_t parameterIndex, std::string_view parameterName, index_t child);

    EvalResult Evaluate(VariableScope& scope) const override;

    std::string ToString() const override;

    std::vector<std::pair<ImVec4, std::string>> GetDrawingSegments(size_t startColorIndex = 0) const override;

  private:
    uint8_t _parameterIndex;
    std::string_view _parameterName;

    friend class FunctionCallExpression;
};

class ValueExpression : public LogicExpression {
  public:
    ValueExpression(function_t function, std::string_view expression, bool isEvent);

    bool IsEvent() const {
        return _isEvent;
    }

    EvalResult Evaluate(VariableScope& scope) const override;

    std::string ToString() const override;

    std::vector<std::pair<ImVec4, std::string>> GetDrawingSegments(size_t startColorIndex = 0) const override;

  private:
    function_t _function;
    std::string_view _expression;
    bool _isEvent;
};

class TernaryExpression : public LogicExpression {
  public:
    TernaryExpression(index_t condition, index_t trueBranch, index_t falseBranch);

    EvalResult Evaluate(VariableScope& scope) const override;

    std::string ToString() const override;

    std::vector<std::pair<ImVec4, std::string>> GetDrawingSegments(size_t startColorIndex = 0) const override;
};

template <typename T = value_t>
    requires (LogicTarget<T> || std::is_same_v<T, value_t>)
class MatchExpression : public LogicExpression {
  public:
    MatchExpression(index_t discriminant, std::vector<index_t> arms);

    EvalResult Evaluate(VariableScope& scope) const override;

    std::string ToString() const override;

    std::vector<std::pair<ImVec4, std::string>> GetDrawingSegments(size_t startColorIndex = 0) const override;

  private:
    static value_t ConvertResult(const value_t& value) {
        if constexpr (std::is_same_v<T, value_t>) {
            return value;
        } else {
            return ConvertValue<T>(value);
        }
    }
};

class MatchArm : public LogicExpression {
  public:
    MatchArm(index_t patterns, index_t expression, bool fallThrough);

    EvalResult Evaluate(VariableScope& scope) const override;

    std::string ToString() const override;

    std::vector<std::pair<ImVec4, std::string>> GetDrawingSegments(size_t startColorIndex = 0) const override;

  private:
    template <typename T>
        requires (LogicTarget<T> || std::is_same_v<T, value_t>)
    friend class MatchExpression;

    EvalResult EvaluateBody(VariableScope& scope) const;

    bool _fallThrough;
};

class MatchArmPatterns : public LogicExpression {
  public:
    MatchArmPatterns(std::vector<index_t> children);

    EvalResult Evaluate(VariableScope& scope) const override;

    std::string ToString() const override;

    std::vector<std::pair<ImVec4, std::string>> GetDrawingSegments(size_t startColorIndex = 0) const override;
};
