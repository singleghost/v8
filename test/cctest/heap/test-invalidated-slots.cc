// Copyright 2017 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <stdlib.h>

#include "src/init/v8.h"

#include "src/heap/heap-inl.h"
#include "src/heap/heap.h"
#include "src/heap/invalidated-slots-inl.h"
#include "src/heap/invalidated-slots.h"
#include "src/heap/store-buffer.h"
#include "test/cctest/cctest.h"
#include "test/cctest/heap/heap-tester.h"
#include "test/cctest/heap/heap-utils.h"

namespace v8 {
namespace internal {
namespace heap {

Page* HeapTester::AllocateByteArraysOnPage(
    Heap* heap, std::vector<ByteArray>* byte_arrays) {
  PauseAllocationObserversScope pause_observers(heap);
  const int kLength = 256 - ByteArray::kHeaderSize;
  const int kSize = ByteArray::SizeFor(kLength);
  CHECK_EQ(kSize, 256);
  Isolate* isolate = heap->isolate();
  PagedSpace* old_space = heap->old_space();
  Page* page;
  // Fill a page with byte arrays.
  {
    AlwaysAllocateScope always_allocate(isolate);
    heap::SimulateFullSpace(old_space);
    ByteArray byte_array;
    CHECK(AllocateByteArrayForTest(heap, kLength, AllocationType::kOld)
              .To(&byte_array));
    byte_arrays->push_back(byte_array);
    page = Page::FromHeapObject(byte_array);
    size_t n = page->area_size() / kSize;
    for (size_t i = 1; i < n; i++) {
      CHECK(AllocateByteArrayForTest(heap, kLength, AllocationType::kOld)
                .To(&byte_array));
      byte_arrays->push_back(byte_array);
      CHECK_EQ(page, Page::FromHeapObject(byte_array));
    }
  }
  CHECK_NULL(page->invalidated_slots<OLD_TO_OLD>());
  return page;
}

template <RememberedSetType direction>
static size_t GetRememberedSetSize(HeapObject obj) {
  std::set<Address> slots;
  RememberedSet<direction>::Iterate(
      MemoryChunk::FromHeapObject(obj),
      [&slots](MaybeObjectSlot slot) {
        slots.insert(slot.address());
        return KEEP_SLOT;
      },
      SlotSet::KEEP_EMPTY_BUCKETS);
  return slots.size();
}

HEAP_TEST(StoreBuffer_CreateFromOldToYoung) {
  CcTest::InitializeVM();
  Isolate* isolate = CcTest::i_isolate();
  Factory* factory = isolate->factory();
  Heap* heap = isolate->heap();
  heap::SealCurrentObjects(heap);
  CHECK(heap->store_buffer()->Empty());

  HandleScope scope(isolate);
  const int n = 10;
  Handle<FixedArray> old = factory->NewFixedArray(n, AllocationType::kOld);

  // Fill the array with refs to both old and new targets.
  {
    const auto prev_top = *(heap->store_buffer_top_address());
    HandleScope scope_inner(isolate);
    intptr_t expected_slots_count = 0;

    // Add refs from old to new.
    for (int i = 0; i < n / 2; i++) {
      Handle<Object> number = factory->NewHeapNumber(i);
      old->set(i, *number);
      expected_slots_count++;
    }
    // Add refs from old to old.
    for (int i = n / 2; i < n; i++) {
      Handle<Object> number = factory->NewHeapNumber<AllocationType::kOld>(i);
      old->set(i, *number);
    }
    // All old to new refs should have been captured and only them.
    const auto new_top = *(heap->store_buffer_top_address());
    const intptr_t added_slots_count =
        (new_top - prev_top) / kSystemPointerSize;
    CHECK_EQ(expected_slots_count, added_slots_count);
  }

  // GC should flush the store buffer into remembered sets and retain the target
  // young objects.
  CHECK_EQ(0, GetRememberedSetSize<OLD_TO_NEW>(*old));
  CcTest::CollectGarbage(i::NEW_SPACE);

  CHECK(heap->store_buffer()->Empty());
  CHECK_EQ(n / 2, GetRememberedSetSize<OLD_TO_NEW>(*old));
  CHECK(Heap::InYoungGeneration(old->get(0)));
}

HEAP_TEST(StoreBuffer_Overflow) {
  CcTest::InitializeVM();
  Isolate* isolate = CcTest::i_isolate();
  Factory* factory = isolate->factory();

  // Add enough refs from old to new to cause overflow of both buffer chunks.
  const int n = 2 * StoreBuffer::kStoreBufferSize / kSystemPointerSize + 1;
  HandleScope scope(isolate);
  Handle<FixedArray> old = factory->NewFixedArray(n, AllocationType::kOld);
  for (int i = 0; i < n; i++) {
    Handle<Object> number = factory->NewHeapNumber(i);
    old->set(i, *number);
  }

  // No test validations, the buffer flipping code triggered by the overflow
  // self-validates with asserts.
}

HEAP_TEST(StoreBuffer_NotUsedOnAgingObjectWithRefsToYounger) {
  CcTest::InitializeVM();
  Isolate* isolate = CcTest::i_isolate();
  Factory* factory = isolate->factory();
  Heap* heap = isolate->heap();
  heap::SealCurrentObjects(heap);
  CHECK(heap->store_buffer()->Empty());

  const int n = 10;
  HandleScope scope(isolate);
  Handle<FixedArray> arr = factory->NewFixedArray(n);

  // Transition the array into the older new tier.
  CcTest::CollectGarbage(i::NEW_SPACE);
  CHECK(Heap::InYoungGeneration(*arr));

  // Fill the array with younger objects.
  {
    HandleScope scope_inner(isolate);
    for (int i = 0; i < n; i++) {
      Handle<Object> number = factory->NewHeapNumber(i);
      arr->set(i, *number);
    }

    // The references aren't crossing generations yet so none should be tracked.
    CHECK(heap->store_buffer()->Empty());
  }

  // Promote the array into old, its elements are still in new, the old to new
  // refs are inserted directly into the remembered sets during GC.
  CcTest::CollectGarbage(i::NEW_SPACE);

  CHECK(heap->InOldSpace(*arr));
  CHECK(Heap::InYoungGeneration(arr->get(n / 2)));
  CHECK(heap->store_buffer()->Empty());
  CHECK_EQ(n, GetRememberedSetSize<OLD_TO_NEW>(*arr));
}

HEAP_TEST(RememberedSet_LargePage) {
  CcTest::InitializeVM();
  Isolate* isolate = CcTest::i_isolate();
  Factory* factory = isolate->factory();
  Heap* heap = isolate->heap();
  heap::SealCurrentObjects(heap);
  CHECK(heap->store_buffer()->Empty());
  v8::HandleScope scope(CcTest::isolate());

  // Allocate an object in Large space.
  const int count = Max(FixedArray::kMaxRegularLength + 1, 128 * KB);
  Handle<FixedArray> arr = factory->NewFixedArray(count, AllocationType::kOld);
  CHECK(heap->lo_space()->Contains(*arr));

  // Create OLD_TO_NEW references from the large object.
  {
    v8::HandleScope short_lived(CcTest::isolate());
    Handle<Object> number = factory->NewHeapNumber(42);
    arr->set(0, *number);
    arr->set(count - 1, *number);
    CHECK(!heap->store_buffer()->Empty());
  }

  // GC should flush the store buffer into the remembered set of the large page,
  // it should also keep the young targets alive.
  CcTest::CollectAllGarbage();

  CHECK(heap->store_buffer()->Empty());
  CHECK(Heap::InYoungGeneration(arr->get(0)));
  CHECK(Heap::InYoungGeneration(arr->get(count - 1)));
  CHECK_EQ(2, GetRememberedSetSize<OLD_TO_NEW>(*arr));
}

HEAP_TEST(InvalidatedSlotsNoInvalidatedRanges) {
  CcTest::InitializeVM();
  Heap* heap = CcTest::heap();
  std::vector<ByteArray> byte_arrays;
  Page* page = AllocateByteArraysOnPage(heap, &byte_arrays);
  InvalidatedSlotsFilter filter = InvalidatedSlotsFilter::OldToOld(page);
  for (ByteArray byte_array : byte_arrays) {
    Address start = byte_array.address() + ByteArray::kHeaderSize;
    Address end = byte_array.address() + byte_array.Size();
    for (Address addr = start; addr < end; addr += kTaggedSize) {
      CHECK(filter.IsValid(addr));
    }
  }
}

HEAP_TEST(InvalidatedSlotsSomeInvalidatedRanges) {
  CcTest::InitializeVM();
  Heap* heap = CcTest::heap();
  std::vector<ByteArray> byte_arrays;
  Page* page = AllocateByteArraysOnPage(heap, &byte_arrays);
  // Register every second byte arrays as invalidated.
  for (size_t i = 0; i < byte_arrays.size(); i += 2) {
    page->RegisterObjectWithInvalidatedSlots<OLD_TO_OLD>(byte_arrays[i]);
  }
  InvalidatedSlotsFilter filter = InvalidatedSlotsFilter::OldToOld(page);
  for (size_t i = 0; i < byte_arrays.size(); i++) {
    ByteArray byte_array = byte_arrays[i];
    Address start = byte_array.address() + ByteArray::kHeaderSize;
    Address end = byte_array.address() + byte_array.Size();
    for (Address addr = start; addr < end; addr += kTaggedSize) {
      if (i % 2 == 0) {
        CHECK(!filter.IsValid(addr));
      } else {
        CHECK(filter.IsValid(addr));
      }
    }
  }
}

HEAP_TEST(InvalidatedSlotsAllInvalidatedRanges) {
  CcTest::InitializeVM();
  Heap* heap = CcTest::heap();
  std::vector<ByteArray> byte_arrays;
  Page* page = AllocateByteArraysOnPage(heap, &byte_arrays);
  // Register the all byte arrays as invalidated.
  for (size_t i = 0; i < byte_arrays.size(); i++) {
    page->RegisterObjectWithInvalidatedSlots<OLD_TO_OLD>(byte_arrays[i]);
  }
  InvalidatedSlotsFilter filter = InvalidatedSlotsFilter::OldToOld(page);
  for (size_t i = 0; i < byte_arrays.size(); i++) {
    ByteArray byte_array = byte_arrays[i];
    Address start = byte_array.address() + ByteArray::kHeaderSize;
    Address end = byte_array.address() + byte_array.Size();
    for (Address addr = start; addr < end; addr += kTaggedSize) {
      CHECK(!filter.IsValid(addr));
    }
  }
}

HEAP_TEST(InvalidatedSlotsAfterTrimming) {
  ManualGCScope manual_gc_scope;
  CcTest::InitializeVM();
  Heap* heap = CcTest::heap();
  std::vector<ByteArray> byte_arrays;
  Page* page = AllocateByteArraysOnPage(heap, &byte_arrays);
  // Register the all byte arrays as invalidated.
  for (size_t i = 0; i < byte_arrays.size(); i++) {
    page->RegisterObjectWithInvalidatedSlots<OLD_TO_OLD>(byte_arrays[i]);
  }
  // Trim byte arrays and check that the slots outside the byte arrays are
  // considered invalid if the old space page was swept.
  InvalidatedSlotsFilter filter = InvalidatedSlotsFilter::OldToOld(page);
  for (size_t i = 0; i < byte_arrays.size(); i++) {
    ByteArray byte_array = byte_arrays[i];
    Address start = byte_array.address() + ByteArray::kHeaderSize;
    Address end = byte_array.address() + byte_array.Size();
    heap->RightTrimFixedArray(byte_array, byte_array.length());
    for (Address addr = start; addr < end; addr += kTaggedSize) {
      CHECK_EQ(filter.IsValid(addr), page->SweepingDone());
    }
  }
}

HEAP_TEST(InvalidatedSlotsEvacuationCandidate) {
  ManualGCScope manual_gc_scope;
  CcTest::InitializeVM();
  Heap* heap = CcTest::heap();
  std::vector<ByteArray> byte_arrays;
  Page* page = AllocateByteArraysOnPage(heap, &byte_arrays);
  page->MarkEvacuationCandidate();
  // Register the all byte arrays as invalidated.
  // This should be no-op because the page is marked as evacuation
  // candidate.
  for (size_t i = 0; i < byte_arrays.size(); i++) {
    page->RegisterObjectWithInvalidatedSlots<OLD_TO_OLD>(byte_arrays[i]);
  }
  // All slots must still be valid.
  InvalidatedSlotsFilter filter = InvalidatedSlotsFilter::OldToOld(page);
  for (size_t i = 0; i < byte_arrays.size(); i++) {
    ByteArray byte_array = byte_arrays[i];
    Address start = byte_array.address() + ByteArray::kHeaderSize;
    Address end = byte_array.address() + byte_array.Size();
    for (Address addr = start; addr < end; addr += kTaggedSize) {
      CHECK(filter.IsValid(addr));
    }
  }
}

HEAP_TEST(InvalidatedSlotsResetObjectRegression) {
  CcTest::InitializeVM();
  Heap* heap = CcTest::heap();
  std::vector<ByteArray> byte_arrays;
  Page* page = AllocateByteArraysOnPage(heap, &byte_arrays);
  // Ensure that the first array has smaller size then the rest.
  heap->RightTrimFixedArray(byte_arrays[0], byte_arrays[0].length() - 8);
  // Register the all byte arrays as invalidated.
  for (size_t i = 0; i < byte_arrays.size(); i++) {
    page->RegisterObjectWithInvalidatedSlots<OLD_TO_OLD>(byte_arrays[i]);
  }
  // All slots must still be invalid.
  InvalidatedSlotsFilter filter = InvalidatedSlotsFilter::OldToOld(page);
  for (size_t i = 0; i < byte_arrays.size(); i++) {
    ByteArray byte_array = byte_arrays[i];
    Address start = byte_array.address() + ByteArray::kHeaderSize;
    Address end = byte_array.address() + byte_array.Size();
    for (Address addr = start; addr < end; addr += kTaggedSize) {
      CHECK(!filter.IsValid(addr));
    }
  }
}

Handle<FixedArray> AllocateArrayOnFreshPage(Isolate* isolate,
                                            PagedSpace* old_space, int length) {
  AlwaysAllocateScope always_allocate(isolate);
  heap::SimulateFullSpace(old_space);
  return isolate->factory()->NewFixedArray(length, AllocationType::kOld);
}

Handle<FixedArray> AllocateArrayOnEvacuationCandidate(Isolate* isolate,
                                                      PagedSpace* old_space,
                                                      int length) {
  Handle<FixedArray> object =
      AllocateArrayOnFreshPage(isolate, old_space, length);
  heap::ForceEvacuationCandidate(Page::FromHeapObject(*object));
  return object;
}

HEAP_TEST(InvalidatedSlotsRightTrimFixedArray) {
  FLAG_manual_evacuation_candidates_selection = true;
  FLAG_parallel_compaction = false;
  ManualGCScope manual_gc_scope;
  CcTest::InitializeVM();
  Isolate* isolate = CcTest::i_isolate();
  Factory* factory = isolate->factory();
  Heap* heap = CcTest::heap();
  HandleScope scope(isolate);
  PagedSpace* old_space = heap->old_space();
  // Allocate a dummy page to be swept be the sweeper during evacuation.
  AllocateArrayOnFreshPage(isolate, old_space, 1);
  Handle<FixedArray> evacuated =
      AllocateArrayOnEvacuationCandidate(isolate, old_space, 1);
  Handle<FixedArray> trimmed = AllocateArrayOnFreshPage(isolate, old_space, 10);
  heap::SimulateIncrementalMarking(heap);
  for (int i = 1; i < trimmed->length(); i++) {
    trimmed->set(i, *evacuated);
  }
  {
    HandleScope scope(isolate);
    Handle<HeapObject> dead = factory->NewFixedArray(1);
    for (int i = 1; i < trimmed->length(); i++) {
      trimmed->set(i, *dead);
    }
    heap->RightTrimFixedArray(*trimmed, trimmed->length() - 1);
  }
  CcTest::CollectGarbage(i::NEW_SPACE);
  CcTest::CollectGarbage(i::OLD_SPACE);
}

HEAP_TEST(InvalidatedSlotsRightTrimLargeFixedArray) {
  FLAG_manual_evacuation_candidates_selection = true;
  FLAG_parallel_compaction = false;
  ManualGCScope manual_gc_scope;
  CcTest::InitializeVM();
  Isolate* isolate = CcTest::i_isolate();
  Factory* factory = isolate->factory();
  Heap* heap = CcTest::heap();
  HandleScope scope(isolate);
  PagedSpace* old_space = heap->old_space();
  // Allocate a dummy page to be swept be the sweeper during evacuation.
  AllocateArrayOnFreshPage(isolate, old_space, 1);
  Handle<FixedArray> evacuated =
      AllocateArrayOnEvacuationCandidate(isolate, old_space, 1);
  Handle<FixedArray> trimmed;
  {
    AlwaysAllocateScope always_allocate(isolate);
    trimmed = factory->NewFixedArray(
        kMaxRegularHeapObjectSize / kTaggedSize + 100, AllocationType::kOld);
    DCHECK(MemoryChunk::FromHeapObject(*trimmed)->InLargeObjectSpace());
  }
  heap::SimulateIncrementalMarking(heap);
  for (int i = 1; i < trimmed->length(); i++) {
    trimmed->set(i, *evacuated);
  }
  {
    HandleScope scope(isolate);
    Handle<HeapObject> dead = factory->NewFixedArray(1);
    for (int i = 1; i < trimmed->length(); i++) {
      trimmed->set(i, *dead);
    }
    heap->RightTrimFixedArray(*trimmed, trimmed->length() - 1);
  }
  CcTest::CollectGarbage(i::NEW_SPACE);
  CcTest::CollectGarbage(i::OLD_SPACE);
}

HEAP_TEST(InvalidatedSlotsLeftTrimFixedArray) {
  FLAG_manual_evacuation_candidates_selection = true;
  FLAG_parallel_compaction = false;
  ManualGCScope manual_gc_scope;
  CcTest::InitializeVM();
  Isolate* isolate = CcTest::i_isolate();
  Factory* factory = isolate->factory();
  Heap* heap = CcTest::heap();
  HandleScope scope(isolate);
  PagedSpace* old_space = heap->old_space();
  // Allocate a dummy page to be swept be the sweeper during evacuation.
  AllocateArrayOnFreshPage(isolate, old_space, 1);
  Handle<FixedArray> evacuated =
      AllocateArrayOnEvacuationCandidate(isolate, old_space, 1);
  Handle<FixedArray> trimmed = AllocateArrayOnFreshPage(isolate, old_space, 10);
  heap::SimulateIncrementalMarking(heap);
  for (int i = 0; i + 1 < trimmed->length(); i++) {
    trimmed->set(i, *evacuated);
  }
  {
    HandleScope scope(isolate);
    Handle<HeapObject> dead = factory->NewFixedArray(1);
    for (int i = 1; i < trimmed->length(); i++) {
      trimmed->set(i, *dead);
    }
    heap->LeftTrimFixedArray(*trimmed, trimmed->length() - 1);
  }
  CcTest::CollectGarbage(i::NEW_SPACE);
  CcTest::CollectGarbage(i::OLD_SPACE);
}

HEAP_TEST(InvalidatedSlotsFastToSlow) {
  FLAG_manual_evacuation_candidates_selection = true;
  FLAG_parallel_compaction = false;
  ManualGCScope manual_gc_scope;
  CcTest::InitializeVM();
  Isolate* isolate = CcTest::i_isolate();
  Factory* factory = isolate->factory();
  Heap* heap = CcTest::heap();
  PagedSpace* old_space = heap->old_space();

  HandleScope scope(isolate);

  Handle<String> name = factory->InternalizeUtf8String("TestObject");
  Handle<String> prop_name1 = factory->InternalizeUtf8String("prop1");
  Handle<String> prop_name2 = factory->InternalizeUtf8String("prop2");
  Handle<String> prop_name3 = factory->InternalizeUtf8String("prop3");
  // Allocate a dummy page to be swept be the sweeper during evacuation.
  AllocateArrayOnFreshPage(isolate, old_space, 1);
  Handle<FixedArray> evacuated =
      AllocateArrayOnEvacuationCandidate(isolate, old_space, 1);
  // Allocate a dummy page to ensure that the JSObject is allocated on
  // a fresh page.
  AllocateArrayOnFreshPage(isolate, old_space, 1);
  Handle<JSObject> obj;
  {
    AlwaysAllocateScope always_allocate(isolate);
    Handle<JSFunction> function = factory->NewFunctionForTest(name);
    function->shared().set_expected_nof_properties(3);
    obj = factory->NewJSObject(function, AllocationType::kOld);
  }
  // Start incremental marking.
  heap::SimulateIncrementalMarking(heap);
  // Set properties to point to the evacuation candidate.
  Object::SetProperty(isolate, obj, prop_name1, evacuated).Check();
  Object::SetProperty(isolate, obj, prop_name2, evacuated).Check();
  Object::SetProperty(isolate, obj, prop_name3, evacuated).Check();

  {
    HandleScope scope(isolate);
    Handle<HeapObject> dead = factory->NewFixedArray(1);
    Object::SetProperty(isolate, obj, prop_name1, dead).Check();
    Object::SetProperty(isolate, obj, prop_name2, dead).Check();
    Object::SetProperty(isolate, obj, prop_name3, dead).Check();
    Handle<Map> map(obj->map(), isolate);
    Handle<Map> normalized_map =
        Map::Normalize(isolate, map, CLEAR_INOBJECT_PROPERTIES, "testing");
    JSObject::MigrateToMap(isolate, obj, normalized_map);
  }
  CcTest::CollectGarbage(i::NEW_SPACE);
  CcTest::CollectGarbage(i::OLD_SPACE);
}

HEAP_TEST(InvalidatedSlotsCleanupFull) {
  ManualGCScope manual_gc_scope;
  CcTest::InitializeVM();
  Heap* heap = CcTest::heap();
  std::vector<ByteArray> byte_arrays;
  Page* page = AllocateByteArraysOnPage(heap, &byte_arrays);
  // Register all byte arrays as invalidated.
  for (size_t i = 0; i < byte_arrays.size(); i++) {
    page->RegisterObjectWithInvalidatedSlots<OLD_TO_NEW>(byte_arrays[i]);
  }

  // Mark full page as free
  InvalidatedSlotsCleanup cleanup = InvalidatedSlotsCleanup::OldToNew(page);
  cleanup.Free(page->area_start(), page->area_end());

  // After cleanup there should be no invalidated objects on page left
  CHECK(page->invalidated_slots<OLD_TO_NEW>()->empty());
}

HEAP_TEST(InvalidatedSlotsCleanupEachObject) {
  ManualGCScope manual_gc_scope;
  CcTest::InitializeVM();
  Heap* heap = CcTest::heap();
  std::vector<ByteArray> byte_arrays;
  Page* page = AllocateByteArraysOnPage(heap, &byte_arrays);
  // Register all byte arrays as invalidated.
  for (size_t i = 0; i < byte_arrays.size(); i++) {
    page->RegisterObjectWithInvalidatedSlots<OLD_TO_NEW>(byte_arrays[i]);
  }

  // Mark each object as free on page
  InvalidatedSlotsCleanup cleanup = InvalidatedSlotsCleanup::OldToNew(page);

  for (size_t i = 0; i < byte_arrays.size(); i++) {
    Address free_start = byte_arrays[i].address();
    Address free_end = free_start + byte_arrays[i].Size();
    cleanup.Free(free_start, free_end);
  }

  // After cleanup there should be no invalidated objects on page left
  CHECK(page->invalidated_slots<OLD_TO_NEW>()->empty());
}

HEAP_TEST(InvalidatedSlotsCleanupRightTrim) {
  ManualGCScope manual_gc_scope;
  CcTest::InitializeVM();
  Heap* heap = CcTest::heap();
  std::vector<ByteArray> byte_arrays;
  Page* page = AllocateByteArraysOnPage(heap, &byte_arrays);

  CHECK_GT(byte_arrays.size(), 1);
  ByteArray& invalidated = byte_arrays[1];

  heap->RightTrimFixedArray(invalidated, invalidated.length() - 8);
  page->RegisterObjectWithInvalidatedSlots<OLD_TO_NEW>(invalidated);

  // Free memory at end of invalidated object
  InvalidatedSlotsCleanup cleanup = InvalidatedSlotsCleanup::OldToNew(page);
  Address free_start = invalidated.address() + invalidated.Size();
  cleanup.Free(free_start, page->area_end());

  // After cleanup the invalidated object should be smaller
  InvalidatedSlots* invalidated_slots = page->invalidated_slots<OLD_TO_NEW>();
  CHECK_EQ(invalidated_slots->size(), 1);
}

}  // namespace heap
}  // namespace internal
}  // namespace v8
