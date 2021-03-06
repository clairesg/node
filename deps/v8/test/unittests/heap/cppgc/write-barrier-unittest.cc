// Copyright 2020 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "include/cppgc/internal/write-barrier.h"

#include <algorithm>
#include <initializer_list>
#include <vector>

#include "include/cppgc/internal/pointer-policies.h"
#include "src/heap/cppgc/heap-object-header.h"
#include "src/heap/cppgc/marker.h"
#include "test/unittests/heap/cppgc/tests.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace cppgc {
namespace internal {

namespace {

class IncrementalMarkingScope {
 public:
  explicit IncrementalMarkingScope(MarkerBase* marker) : marker_(marker) {
    marker_->StartMarking(kIncrementalConfig);
  }

  ~IncrementalMarkingScope() V8_NOEXCEPT {
    marker_->FinishMarking(kIncrementalConfig);
  }

 private:
  static constexpr Marker::MarkingConfig kIncrementalConfig{
      Marker::MarkingConfig::CollectionType::kMajor,
      Marker::MarkingConfig::StackState::kNoHeapPointers,
      Marker::MarkingConfig::MarkingType::kIncremental};

  MarkerBase* marker_;
};

constexpr Marker::MarkingConfig IncrementalMarkingScope::kIncrementalConfig;

class ExpectWriteBarrierFires final : private IncrementalMarkingScope {
 public:
  ExpectWriteBarrierFires(MarkerBase* marker,
                          std::initializer_list<void*> objects)
      : IncrementalMarkingScope(marker),
        marking_worklist_(
            marker->MarkingWorklistsForTesting().marking_worklist(),
            MarkingWorklists::kMutatorThreadId),
        write_barrier_worklist_(
            marker->MarkingWorklistsForTesting().write_barrier_worklist(),
            MarkingWorklists::kMutatorThreadId),
        objects_(objects) {
    EXPECT_TRUE(marking_worklist_.IsGlobalPoolEmpty());
    EXPECT_TRUE(write_barrier_worklist_.IsGlobalPoolEmpty());
    for (void* object : objects) {
      headers_.push_back(&HeapObjectHeader::FromPayload(object));
      EXPECT_FALSE(headers_.back()->IsMarked());
    }
  }

  ~ExpectWriteBarrierFires() V8_NOEXCEPT {
    {
      MarkingWorklists::MarkingItem item;
      while (marking_worklist_.Pop(&item)) {
        auto pos = std::find(objects_.begin(), objects_.end(),
                             item.base_object_payload);
        if (pos != objects_.end()) objects_.erase(pos);
      }
    }
    {
      HeapObjectHeader* item;
      while (write_barrier_worklist_.Pop(&item)) {
        auto pos = std::find(objects_.begin(), objects_.end(), item->Payload());
        if (pos != objects_.end()) objects_.erase(pos);
      }
    }
    EXPECT_TRUE(objects_.empty());
    for (auto* header : headers_) {
      EXPECT_TRUE(header->IsMarked());
      header->Unmark();
    }
    EXPECT_TRUE(marking_worklist_.IsGlobalPoolEmpty());
    EXPECT_TRUE(write_barrier_worklist_.IsGlobalPoolEmpty());
  }

 private:
  MarkingWorklists::MarkingWorklist::View marking_worklist_;
  MarkingWorklists::WriteBarrierWorklist::View write_barrier_worklist_;
  std::vector<void*> objects_;
  std::vector<HeapObjectHeader*> headers_;
};

class ExpectNoWriteBarrierFires final : private IncrementalMarkingScope {
 public:
  ExpectNoWriteBarrierFires(MarkerBase* marker,
                            std::initializer_list<void*> objects)
      : IncrementalMarkingScope(marker),
        marking_worklist_(
            marker->MarkingWorklistsForTesting().marking_worklist(),
            MarkingWorklists::kMutatorThreadId),
        write_barrier_worklist_(
            marker->MarkingWorklistsForTesting().write_barrier_worklist(),
            MarkingWorklists::kMutatorThreadId) {
    EXPECT_TRUE(marking_worklist_.IsGlobalPoolEmpty());
    EXPECT_TRUE(write_barrier_worklist_.IsGlobalPoolEmpty());
    for (void* object : objects) {
      auto* header = &HeapObjectHeader::FromPayload(object);
      headers_.emplace_back(header, header->IsMarked());
    }
  }

  ~ExpectNoWriteBarrierFires() {
    EXPECT_TRUE(marking_worklist_.IsGlobalPoolEmpty());
    EXPECT_TRUE(write_barrier_worklist_.IsGlobalPoolEmpty());
    for (const auto& pair : headers_) {
      EXPECT_EQ(pair.second, pair.first->IsMarked());
    }
  }

 private:
  MarkingWorklists::MarkingWorklist::View marking_worklist_;
  MarkingWorklists::WriteBarrierWorklist::View write_barrier_worklist_;
  std::vector<std::pair<HeapObjectHeader*, bool /* was marked */>> headers_;
};

class GCed : public GarbageCollected<GCed> {
 public:
  GCed() = default;
  explicit GCed(GCed* next) : next_(next) {}

  void Trace(cppgc::Visitor* v) const { v->Trace(next_); }

  bool IsMarked() const {
    return HeapObjectHeader::FromPayload(this).IsMarked();
  }

  void set_next(GCed* next) { next_ = next; }
  GCed* next() const { return next_; }
  Member<GCed>& next_ref() { return next_; }

 private:
  Member<GCed> next_ = nullptr;
};

}  // namespace

class WriteBarrierTest : public testing::TestWithHeap {
 public:
  WriteBarrierTest() : internal_heap_(Heap::From(GetHeap())) {
    GetMarkerRef() = std::make_unique<Marker>(internal_heap_->AsBase());
    marker_ = GetMarkerRef().get();
  }

  ~WriteBarrierTest() override {
    marker_->ClearAllWorklistsForTesting();
    GetMarkerRef().reset();
  }

  MarkerBase* marker() const { return marker_; }

 private:
  Heap* internal_heap_;
  MarkerBase* marker_;
};

// =============================================================================
// Basic support. ==============================================================
// =============================================================================

TEST_F(WriteBarrierTest, EnableDisableIncrementalMarking) {
  {
    IncrementalMarkingScope scope(marker());
    EXPECT_TRUE(ProcessHeap::IsAnyIncrementalOrConcurrentMarking());
  }
}

TEST_F(WriteBarrierTest, TriggersWhenMarkingIsOn) {
  auto* object1 = MakeGarbageCollected<GCed>(GetAllocationHandle());
  auto* object2 = MakeGarbageCollected<GCed>(GetAllocationHandle());
  {
    ExpectWriteBarrierFires scope(marker(), {object1});
    EXPECT_FALSE(object1->IsMarked());
    object2->set_next(object1);
    EXPECT_TRUE(object1->IsMarked());
  }
}

TEST_F(WriteBarrierTest, BailoutWhenMarkingIsOff) {
  auto* object1 = MakeGarbageCollected<GCed>(GetAllocationHandle());
  auto* object2 = MakeGarbageCollected<GCed>(GetAllocationHandle());
  EXPECT_FALSE(object1->IsMarked());
  object2->set_next(object1);
  EXPECT_FALSE(object1->IsMarked());
}

TEST_F(WriteBarrierTest, BailoutIfMarked) {
  auto* object1 = MakeGarbageCollected<GCed>(GetAllocationHandle());
  auto* object2 = MakeGarbageCollected<GCed>(GetAllocationHandle());
  EXPECT_TRUE(HeapObjectHeader::FromPayload(object1).TryMarkAtomic());
  {
    ExpectNoWriteBarrierFires scope(marker(), {object1});
    object2->set_next(object1);
  }
}

TEST_F(WriteBarrierTest, MemberInitializingStoreNoBarrier) {
  auto* object1 = MakeGarbageCollected<GCed>(GetAllocationHandle());
  {
    ExpectNoWriteBarrierFires scope(marker(), {object1});
    auto* object2 = MakeGarbageCollected<GCed>(GetAllocationHandle(), object1);
    HeapObjectHeader& object2_header = HeapObjectHeader::FromPayload(object2);
    EXPECT_FALSE(object2_header.IsMarked());
  }
}

TEST_F(WriteBarrierTest, MemberReferenceAssignMember) {
  auto* obj = MakeGarbageCollected<GCed>(GetAllocationHandle());
  auto* ref_obj = MakeGarbageCollected<GCed>(GetAllocationHandle());
  Member<GCed>& m2 = ref_obj->next_ref();
  Member<GCed> m3(obj);
  {
    ExpectWriteBarrierFires scope(marker(), {obj});
    m2 = m3;
  }
}

TEST_F(WriteBarrierTest, MemberSetSentinelValueNoBarrier) {
  auto* obj = MakeGarbageCollected<GCed>(GetAllocationHandle());
  Member<GCed>& m = obj->next_ref();
  {
    ExpectNoWriteBarrierFires scope(marker(), {});
    m = kSentinelPointer;
  }
}

TEST_F(WriteBarrierTest, MemberCopySentinelValueNoBarrier) {
  auto* obj1 = MakeGarbageCollected<GCed>(GetAllocationHandle());
  Member<GCed>& m1 = obj1->next_ref();
  m1 = kSentinelPointer;
  {
    ExpectNoWriteBarrierFires scope(marker(), {});
    auto* obj2 = MakeGarbageCollected<GCed>(GetAllocationHandle());
    obj2->next_ref() = m1;
  }
}

// =============================================================================
// Mixin support. ==============================================================
// =============================================================================

namespace {

class Mixin : public GarbageCollectedMixin {
 public:
  void Trace(cppgc::Visitor* visitor) const override { visitor->Trace(next_); }

  virtual void Bar() {}

 protected:
  Member<GCed> next_;
};

class ClassWithVirtual {
 protected:
  virtual void Foo() {}
};

class Child : public GarbageCollected<Child>,
              public ClassWithVirtual,
              public Mixin {
 public:
  Child() : ClassWithVirtual(), Mixin() {}
  ~Child() = default;

  void Trace(cppgc::Visitor* visitor) const override { Mixin::Trace(visitor); }

  void Foo() override {}
  void Bar() override {}
};

class ParentWithMixinPointer : public GarbageCollected<ParentWithMixinPointer> {
 public:
  ParentWithMixinPointer() = default;

  void set_mixin(Mixin* mixin) { mixin_ = mixin; }

  virtual void Trace(cppgc::Visitor* visitor) const { visitor->Trace(mixin_); }

 protected:
  Member<Mixin> mixin_;
};

}  // namespace

TEST_F(WriteBarrierTest, WriteBarrierOnUnmarkedMixinApplication) {
  ParentWithMixinPointer* parent =
      MakeGarbageCollected<ParentWithMixinPointer>(GetAllocationHandle());
  auto* child = MakeGarbageCollected<Child>(GetAllocationHandle());
  Mixin* mixin = static_cast<Mixin*>(child);
  EXPECT_NE(static_cast<void*>(child), static_cast<void*>(mixin));
  {
    ExpectWriteBarrierFires scope(marker(), {child});
    parent->set_mixin(mixin);
  }
}

TEST_F(WriteBarrierTest, NoWriteBarrierOnMarkedMixinApplication) {
  ParentWithMixinPointer* parent =
      MakeGarbageCollected<ParentWithMixinPointer>(GetAllocationHandle());
  auto* child = MakeGarbageCollected<Child>(GetAllocationHandle());
  EXPECT_TRUE(HeapObjectHeader::FromPayload(child).TryMarkAtomic());
  Mixin* mixin = static_cast<Mixin*>(child);
  EXPECT_NE(static_cast<void*>(child), static_cast<void*>(mixin));
  {
    ExpectNoWriteBarrierFires scope(marker(), {child});
    parent->set_mixin(mixin);
  }
}

}  // namespace internal
}  // namespace cppgc
