#pragma once
#include <array>
#include <cstddef>
#include <optional>
#include <utility>

namespace hitbox {
// Caller supplies synchronization. Fixed storage avoids allocating on game ticks.
template<class T, std::size_t Capacity> class BatchQueue {
    static_assert(Capacity>0);
public:
    bool push(const T& value) {
        if(count_==Capacity) return false;
        slots_[(head_+count_)%Capacity]=value; ++count_; return true;
    }
    std::optional<T> pop() {
        if(!count_) return {};
        auto value=std::move(slots_[head_]); slots_[head_].reset();
        head_=(head_+1)%Capacity; --count_; return value;
    }
    bool empty() const noexcept { return count_==0; }
    void clear() { for(auto& slot:slots_) slot.reset(); head_=count_=0; }
private:
    std::array<std::optional<T>,Capacity> slots_{};
    std::size_t head_{},count_{};
};
// Keep an already captured batch until EVERY shard accepts it. Do not capture
// further rows while blocked, and never send the same batch twice to one shard.
template<class T,std::size_t Shards> class PendingDelivery {
public:
    bool begin(T value) {
        if(value_) return false;
        value_=std::move(value); delivered_.fill(false); return true;
    }
    template<class Send> bool flush(Send&& send) {
        if(!value_) return true;
        bool complete=true;
        for(std::size_t i=0;i<Shards;++i) {
            if(!delivered_[i]) delivered_[i]=send(i,*value_);
            complete=complete && delivered_[i];
        }
        if(complete) value_.reset();
        return complete;
    }
    void clear() { value_.reset(); delivered_.fill(false); }
private:
    std::optional<T> value_;
    std::array<bool,Shards> delivered_{};
};
}
