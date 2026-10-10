#pragma once
#include <iterator>
#include <memory>
#include <utility>
#include <vector>

namespace War3Map::Render {
// 仅供进程内静态场景存储；const 读取共享，任何可写访问先独占存储。
// 可写迭代器/引用的有效期遵循 vector；不得把它们保留到另一个场景快照发布之后。
template<class T> class SharedSceneVector {
    using Storage = std::vector<T>;
    std::shared_ptr<Storage> storage_;
    Storage& Write() {
        if (!storage_) storage_ = std::make_shared<Storage>();
        else if (storage_.use_count() != 1) storage_ = std::make_shared<Storage>(*storage_);
        return *storage_;
    }
    const Storage& Read() const noexcept {
        static const Storage empty;
        return storage_ ? *storage_ : empty;
    }
public:
    using value_type=T; using size_type=typename Storage::size_type;
    using difference_type=typename Storage::difference_type;
    using iterator=typename Storage::iterator; using const_iterator=typename Storage::const_iterator;
    using reverse_iterator=typename Storage::reverse_iterator;
    using const_reverse_iterator=typename Storage::const_reverse_iterator;
    SharedSceneVector() = default;
    SharedSceneVector(Storage values) : storage_(std::make_shared<Storage>(std::move(values))) {}
    SharedSceneVector(std::initializer_list<T> values) : SharedSceneVector(Storage(values)) {}
    SharedSceneVector& operator=(Storage values) { storage_=std::make_shared<Storage>(std::move(values)); return *this; }
    SharedSceneVector& operator=(std::initializer_list<T> values) { return *this=Storage(values); }
    operator const Storage&() const noexcept { return Read(); }
    operator Storage&() { return Write(); }
    [[nodiscard]] bool SharesStorageWith(const SharedSceneVector& other) const noexcept { return storage_==other.storage_; }
    [[nodiscard]] size_type size() const noexcept { return Read().size(); }
    [[nodiscard]] size_type capacity() const noexcept { return Read().capacity(); }
    [[nodiscard]] bool empty() const noexcept { return Read().empty(); }
    const T* data() const noexcept { return Read().data(); } T* data() { return Write().data(); }
    const_iterator begin() const noexcept { return Read().begin(); } iterator begin() { return Write().begin(); }
    const_iterator end() const noexcept { return Read().end(); } iterator end() { return Write().end(); }
    const_iterator cbegin() const noexcept { return Read().cbegin(); }
    const_iterator cend() const noexcept { return Read().cend(); }
    const_reverse_iterator rbegin() const noexcept { return Read().rbegin(); } reverse_iterator rbegin() { return Write().rbegin(); }
    const_reverse_iterator rend() const noexcept { return Read().rend(); } reverse_iterator rend() { return Write().rend(); }
    const T& operator[](size_type i) const noexcept { return Read()[i]; } T& operator[](size_type i) { return Write()[i]; }
    const T& at(size_type i) const { return Read().at(i); } T& at(size_type i) { return Write().at(i); }
    const T& front() const { return Read().front(); } T& front() { return Write().front(); }
    const T& back() const { return Read().back(); } T& back() { return Write().back(); }
    void clear() { if (storage_ && storage_.use_count() == 1) storage_->clear(); else storage_.reset(); }
    void reserve(size_type n) { Write().reserve(n); }
    void resize(size_type n) { Write().resize(n); } void resize(size_type n,const T& value) { Write().resize(n,value); }
    void shrink_to_fit() { Write().shrink_to_fit(); }
    void push_back(const T& value) { Write().push_back(value); } void push_back(T&& value) { Write().push_back(std::move(value)); }
    template<class... Args> T& emplace_back(Args&&... args) { return Write().emplace_back(std::forward<Args>(args)...); }
    void pop_back() { Write().pop_back(); }
    void assign(size_type n,const T& value) { Write().assign(n,value); }
    template<std::input_iterator It> void assign(It first,It last) { Write().assign(first,last); }
    void assign(std::initializer_list<T> values) { Write().assign(values); }
    iterator insert(const_iterator pos,const T& value) { const auto n=pos-Read().cbegin(); auto& v=Write(); return v.insert(v.cbegin()+n,value); }
    iterator insert(const_iterator pos,T&& value) { const auto n=pos-Read().cbegin(); auto& v=Write(); return v.insert(v.cbegin()+n,std::move(value)); }
    iterator insert(const_iterator pos,size_type count,const T& value) { const auto n=pos-Read().cbegin(); auto& v=Write(); return v.insert(v.cbegin()+n,count,value); }
    template<std::input_iterator It> iterator insert(const_iterator pos,It first,It last) { const auto n=pos-Read().cbegin(); auto& v=Write(); return v.insert(v.cbegin()+n,first,last); }
    iterator erase(const_iterator pos) { const auto n=pos-Read().cbegin(); auto& v=Write(); return v.erase(v.cbegin()+n); }
    iterator erase(const_iterator first,const_iterator last) { const auto a=first-Read().cbegin(),b=last-Read().cbegin(); auto& v=Write(); return v.erase(v.cbegin()+a,v.cbegin()+b); }
    void swap(SharedSceneVector& other) noexcept { storage_.swap(other.storage_); }
    void swap(Storage& other) { Write().swap(other); }
    friend bool operator==(const SharedSceneVector& a,const SharedSceneVector& b) { return a.Read()==b.Read(); }
    friend bool operator==(const SharedSceneVector& a,const Storage& b) { return a.Read()==b; }
    friend bool operator==(const Storage& a,const SharedSceneVector& b) { return a==b.Read(); }
};
}
