#pragma once

#include <optional>
#include <utility>

namespace ce {
// The backend's process environment lock covers each read/write transaction.
// A subsequent application write becomes the baseline for a new policy, and
// deactivation restores only a value still owned by this override.
template<class Value>
class OwnedEnvironmentValue {
public:
    template<class Read, class Write>
    bool Apply(Value value, Read read, Write write) {
        if (owned_ && value == last_) return true;
        const auto current = read();
        if (!owned_ || current != last_) original_ = current;
        if (!write(std::optional<Value>{value})) return false;
        last_ = std::move(value);
        owned_ = true;
        return true;
    }
    template<class Read, class Write>
    bool Restore(Read read, Write write) {
        if (!owned_) return true;
        if (read() == last_ && !write(original_)) return false;
        owned_ = false;
        original_.reset();
        last_.reset();
        return true;
    }
private:
    bool owned_ = false;
    std::optional<Value> original_, last_;
};
}  // namespace ce
