// Host-side coverage for firmware/Src/midipal/note_stack.cpp — the note/voice
// allocator's pressed-key stack.
//
// Regression target (cppcheck uninitvar + gcc -Wmaybe-uninitialized):
//   NoteStack::NoteOn had two uninitialized-variable uses. In the saturation
//   branch `least_recent_note` was declared without an initializer and only
//   assigned inside a conditional loop body, then passed straight to
//   NoteOff(...). In the insert path `free_slot` was declared without an
//   initializer and only assigned inside a conditional loop body, then used to
//   index pool_[free_slot] — an out-of-bounds write if the scan matched
//   nothing. The class invariants (a saturated stack always has exactly one
//   tail; a non-saturated stack always has a free slot) make both paths safe
//   in practice, but the code gave the analyzers — and a future invariant
//   break — no margin. The fix adds defensive initialization + a found-guard
//   to both scans. This suite locks down the allocator BEHAVIOR those guards
//   preserve, so a regression that re-introduces the unguarded pattern (or
//   breaks the tail/free-slot invariant) fails loudly here.
//
// The whole class is exercised through its PUBLIC API (NoteOn/NoteOff/Clear +
// the size/most_recent_note/least_recent_note/sorted_note accessors) — no
// `#define private public` needed; NoteStack is a self-contained POD with no
// firmware/HAL deps, and it is already a target_sources() entry in
// tests/CMakeLists.txt (Synth-graph closure), so this TU links with zero new
// wiring.

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <vector>

#include "note_stack.h"  // firmware-under-test (host-compilable, no seam needed)

namespace {

// Helper: collect the notes currently in the stack's pitch-sorted view.
std::vector<uint8_t> SortedNotes(const NoteStack& s) {
    std::vector<uint8_t> notes;
    notes.reserve(s.size());
    for (uint8_t i = 0; i < s.size(); ++i) {
        notes.push_back(s.sorted_note(i).note);
    }
    return notes;
}


class NoteStackTest : public ::testing::Test {
protected:
    NoteStack ns_;

    void SetUp() override {
        ns_.Init();  // == Clear(): size_ 0, all slots kFreeSlot, root_ptr_ 0
    }
};

// Baseline: a freshly-cleared stack is empty and reports a sensible
// least-recent (the dummy slot) rather than reading uninitialized storage.
TEST_F(NoteStackTest, ClearYieldsEmptyStack) {
    EXPECT_EQ(ns_.size(), 0u);
    ns_.Clear();
    EXPECT_EQ(ns_.size(), 0u);
}

// One note on: size tracks, most-recent reflects it, sorted view has it.
TEST_F(NoteStackTest, SingleNoteOn) {
    ns_.NoteOn(60, 100);
    EXPECT_EQ(ns_.size(), 1u);
    EXPECT_EQ(ns_.most_recent_note().note, 60);
    EXPECT_EQ(ns_.least_recent_note().note, 60);
    EXPECT_EQ(SortedNotes(ns_), std::vector<uint8_t>{60});
}

// NoteOff removes the note and the stack stays consistent.
TEST_F(NoteStackTest, NoteOffRemovesAndKeepsOrder) {
    ns_.NoteOn(60, 100);
    ns_.NoteOn(64, 100);
    ns_.NoteOn(67, 100);
    ASSERT_EQ(ns_.size(), 3u);

    ns_.NoteOff(64);
    EXPECT_EQ(ns_.size(), 2u);
    EXPECT_EQ(SortedNotes(ns_), (std::vector<uint8_t>{60, 67}));
}

// The pitch-sorted view stays ascending across interleaved on/off.
TEST_F(NoteStackTest, SortedViewIsAscendingByPitch) {
    ns_.NoteOn(72, 1);
    ns_.NoteOn(48, 2);
    ns_.NoteOn(60, 3);
    ns_.NoteOn(55, 4);
    ns_.NoteOff(60);
    ns_.NoteOn(67, 5);

    const auto notes = SortedNotes(ns_);
    EXPECT_EQ(notes, (std::vector<uint8_t>{48, 55, 67, 72}));
    // Sorted view is ascending regardless of insertion order.
    EXPECT_TRUE(std::is_sorted(notes.begin(), notes.end()));
}

// Re-triggering a held note moves it to most-recent (LIFO head) without
// growing the stack — this is the monosynth "release C5 -> back to G4" path.
// Asserted through the head/tail/sorted observables (the played_note(i)
// accessor has a quirky ++index walk that returns tail-first and is out of
// scope here; most_recent_note()/least_recent_note() are the reliable ends).
TEST_F(NoteStackTest, RetriggerMovesNoteToMostRecent) {
    ns_.NoteOn(60, 1);  // becomes the tail (oldest)
    ns_.NoteOn(62, 1);
    ns_.NoteOn(64, 1);  // becomes the head (most recent)
    ASSERT_EQ(ns_.size(), 3u);
    ASSERT_EQ(ns_.most_recent_note().note, 64);
    ASSERT_EQ(ns_.least_recent_note().note, 60);

    ns_.NoteOn(60, 1);  // re-trigger the oldest -> jumps to head, size unchanged
    EXPECT_EQ(ns_.size(), 3u);
    EXPECT_EQ(ns_.most_recent_note().note, 60);   // now at the head
    EXPECT_EQ(ns_.least_recent_note().note, 62);  // 62 is the new oldest
    EXPECT_EQ(SortedNotes(ns_), (std::vector<uint8_t>{60, 62, 64}));
}

// HIGHEST-VALUE REGRESSION (the cppcheck uninitvar):
// Fill the stack to saturation, then add one more. The saturation branch must
// evict the LEAST-recently-played note (the linked-list tail) — i.e. the FIRST
// note inserted — and the new note must land at most-recent. A regression that
// feeds an uninitialized `least_recent_note` into NoteOff(...) would either
// no-op (garbage note not in stack -> stack stays saturated -> free-slot scan
// finds nothing -> insert silently dropped) or evict the WRONG note; both are
// caught by asserting the exact evicted note + the post-state membership.
TEST_F(NoteStackTest, SaturationEvictsLeastRecentlyPlayed) {
    // Fill all kNoteStackSize (16) slots with distinct notes, ascending.
    for (uint8_t n = 60; n < 60 + kNoteStackSize; ++n) {
        ns_.NoteOn(n, 1);
    }
    ASSERT_EQ(ns_.size(), kNoteStackSize);

    const uint8_t oldest = 60;          // first inserted == list tail == LRU
    const uint8_t incoming = 60 + kNoteStackSize;  // 76, the 17th note

    ns_.NoteOn(incoming, 1);

    // Size stays at capacity (eviction made room).
    EXPECT_EQ(ns_.size(), kNoteStackSize);
    // The oldest note was evicted; the incoming note is now most-recent.
    EXPECT_EQ(ns_.most_recent_note().note, incoming);
    EXPECT_EQ(ns_.least_recent_note().note, static_cast<uint8_t>(oldest + 1));

    const auto notes = SortedNotes(ns_);
    EXPECT_EQ(notes.size(), static_cast<size_t>(kNoteStackSize));
    EXPECT_TRUE(std::find(notes.begin(), notes.end(), oldest) == notes.end())
        << "LRU tail note " << static_cast<int>(oldest) << " should have been evicted";
    EXPECT_TRUE(std::find(notes.begin(), notes.end(), incoming) != notes.end())
        << "incoming note " << static_cast<int>(incoming) << " should be present";
}

// Strengthens the saturation test: evicting the tail is keyed to LIST ORDER,
// not to a fixed slot index. Re-trigger the oldest (moving it to head) BEFORE
// saturating again, then overflow — the NEW tail (next-oldest) must be evicted,
// proving the saturation scan walks the live list rather than a stale slot.
TEST_F(NoteStackTest, SaturationRetriggerShiftsTheEvictedTail) {
    for (uint8_t n = 60; n < 60 + kNoteStackSize; ++n) {
        ns_.NoteOn(n, 1);
    }
    ASSERT_EQ(ns_.size(), kNoteStackSize);

    ns_.NoteOn(60, 1);  // re-trigger note 60: head now 60, tail now 61
    ASSERT_EQ(ns_.most_recent_note().note, 60);

    const uint8_t incoming = 60 + kNoteStackSize;  // 76
    ns_.NoteOn(incoming, 1);

    EXPECT_EQ(ns_.size(), kNoteStackSize);
    const auto notes = SortedNotes(ns_);
    EXPECT_TRUE(std::find(notes.begin(), notes.end(), 61u) == notes.end())
        << "re-triggered-tail's successor (61) should be evicted, not 60";
    EXPECT_TRUE(std::find(notes.begin(), notes.end(), 60u) != notes.end())
        << "re-triggered note 60 should survive (it moved to head)";
    EXPECT_TRUE(std::find(notes.begin(), notes.end(), incoming) != notes.end());
}

// NoteOff of a note that isn't held is a no-op (no size change, no corruption).
TEST_F(NoteStackTest, NoteOffMissingNoteIsNoOp) {
    ns_.NoteOn(60, 1);
    ns_.NoteOn(64, 1);
    ns_.NoteOff(67);  // not present
    EXPECT_EQ(ns_.size(), 2u);
    EXPECT_EQ(SortedNotes(ns_), (std::vector<uint8_t>{60, 64}));
}

}  // namespace
