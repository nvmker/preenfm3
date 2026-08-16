// Host-side coverage for firmware/Src/midipal/event_scheduler.cpp — the
// arpeggiator/sequencer event scheduler (sorted delay-list of note events).
//
// Regression target (per test-coverage-plan.md Phase 1, row 1):
//   Stuck/dropped scheduled notes. EventScheduler is the note-event queue the
//   arpeggiator and step-sequencer timing run on: Schedule inserts events into
//   a singly-linked list kept sorted by `when` ascending, Tick decrements every
//   pending event and frees the head when its delay hits zero, Remove tags a
//   matching (note,velocity) entry as a zombie (kZombieSlot) so the slot is
//   NOT reused until Tick frees it, and overflow()/the silent full-queue
//   no-op bound memory. A regression in the insertion order, the tick/free
//   walk, or the zombie protocol fails loudly here.
//
// The whole class is exercised through its PUBLIC API (Init/Tick/Schedule/
// Remove + entry()/root()/size()/overflow() accessors) — no `#define private
// public` needed; EventScheduler is a self-contained POD-ish class with no
// firmware/HAL deps, already a target_sources() entry in tests/CMakeLists.txt
// (Synth-graph closure from Target #4).
//
// KNOWN LATENT HAZARD characterized, NOT driven (see
// EventScheduler.Note0xffIsStoredAndFreesNormally): kFreeSlot==0xff equals the
// note value 0xff, so an entry SCHEDULED with note==0xff is indistinguishable
// from a free slot to the allocator's free-slot scan. Any later Schedule()
// that reuses that still-linked slot links it onto itself (self-loop / 2-cycle
// list), which makes Tick()'s decrement walk loop forever (hang). The safe,
// observable half is locked: a 0xff note is stored, counted in size(), and
// freed normally by Tick(); the reuse half is documented here because driving
// it would hang the suite. Real firmware notes are 0..127, so the collision
// is unreachable in practice. Flagged for a separate firmware change.

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "event_scheduler.h"  // firmware-under-test (host-compilable, no seam needed)

namespace {

// Walk the linked list from root, collecting (note, velocity, when) per entry.
// Used to assert list ORDER (sorted by `when` ascending) and membership.
struct EntryView {
    uint8_t note, velocity, when;
};
std::vector<EntryView> ListContents(EventScheduler& es) {
    std::vector<EntryView> out;
    bool seen[kEventSchedulerSize] = {};
    uint8_t cur = es.root();
    while (cur != 0) {
        if (cur >= kEventSchedulerSize) {
            ADD_FAILURE() << "scheduler list contains invalid slot " << (int)cur;
            break;
        }
        if (seen[cur]) {
            ADD_FAILURE() << "scheduler list contains a cycle at slot " << (int)cur;
            break;
        }
        seen[cur] = true;
        out.push_back({es.entry(cur).note, es.entry(cur).velocity,
                       es.entry(cur).when});
        cur = es.entry(cur).next;
    }
    return out;
}

class EventSchedulerTest : public ::testing::Test {
protected:
    EventScheduler es_;

    void SetUp() override {
        es_.Init();  // all entries kFreeSlot, root_ptr_ 0, size_ 0
    }
};

// A freshly-Init'ed scheduler is empty: size 0, root 0 (the sentinel slot 0 is
// never used for entries — the free-slot scan starts at index 1), and every
// slot tagged free. Re-Init after use restores the same state.
TEST_F(EventSchedulerTest, InitYieldsEmptyQueue) {
    EXPECT_EQ(es_.size(), 0);
    EXPECT_EQ(es_.root(), 0);
    EXPECT_EQ(es_.entry(0).note, kFreeSlot);
    for (int i = 1; i < kEventSchedulerSize; i += 7) {
        EXPECT_EQ(es_.entry(i).note, kFreeSlot);
    }
    es_.Schedule(60, 1, 5);
    ASSERT_EQ(es_.size(), 1);
    es_.Init();
    EXPECT_EQ(es_.size(), 0);
    EXPECT_EQ(es_.root(), 0);
}

// Schedule inserts keeping the root list sorted by `when` ASCENDING, whatever
// the arrival order — the head is always the next event to fire. Out-of-order
// arrival exercises both insert paths: new head (when < root) and mid/tail
// walk (when >= root, advance while next exists and when >= next.when).
TEST_F(EventSchedulerTest, ScheduleKeepsListSortedByWhenAscending) {
    es_.Schedule(60, 100, 10);  // first entry: empty-list branch (root_ptr_==0)
    es_.Schedule(62, 100, 5);   // earlier than root -> new head
    es_.Schedule(64, 100, 7);   // between 5 and 10 -> mid insert (the walk)
    es_.Schedule(66, 100, 20);  // later than all -> tail insert
    ASSERT_EQ(es_.size(), 4);
    const auto list = ListContents(es_);
    ASSERT_EQ(list.size(), 4u);
    EXPECT_EQ(list[0].note, 62); EXPECT_EQ(list[0].when, 5);
    EXPECT_EQ(list[1].note, 64); EXPECT_EQ(list[1].when, 7);
    EXPECT_EQ(list[2].note, 60); EXPECT_EQ(list[2].when, 10);
    EXPECT_EQ(list[3].note, 66); EXPECT_EQ(list[3].when, 20);
    // size() counts entries, root() is the earliest.
    EXPECT_EQ(es_.entry(es_.root()).when, 5);
}

// Equal `when` values keep arrival (FIFO) order: the walk's condition is
// `when >= entries[next].when`, so an equal-when newcomer advances PAST the
// existing entry and is appended after it.
TEST_F(EventSchedulerTest, EqualWhenInsertsArriveInFifoOrder) {
    es_.Schedule(60, 1, 5);
    es_.Schedule(62, 1, 5);
    es_.Schedule(64, 1, 5);
    const auto list = ListContents(es_);
    ASSERT_EQ(list.size(), 3u);
    EXPECT_EQ(list[0].note, 60);
    EXPECT_EQ(list[1].note, 62);
    EXPECT_EQ(list[2].note, 64);
}

// An event scheduled with when=k is decremented once per Tick and FREED on the
// (k+1)-th Tick. Between ticks it stays live and counted.
TEST_F(EventSchedulerTest, TickDecrementsAndFreesWhenZero) {
    es_.Schedule(60, 1, 3);
    ASSERT_EQ(es_.size(), 1);
    for (int t = 1; t <= 3; t++) {
        es_.Tick();
        ASSERT_EQ(es_.size(), 1) << "still pending after tick " << t;
        ASSERT_EQ(es_.entry(es_.root()).when, 3 - t);
        ASSERT_EQ(es_.entry(es_.root()).note, 60);
    }
    es_.Tick();  // 4th tick: when hits 0 -> freed by the head-free loop
    EXPECT_EQ(es_.size(), 0);
    EXPECT_EQ(es_.root(), 0);
    EXPECT_EQ(es_.entry(es_.root() == 0 ? 1 : es_.root()).note, kFreeSlot);
}

// The head-free loop is a WHILE: multiple when==0 entries chained at the head
// are all freed in a single Tick. (when=0 entries sort to the head.)
TEST_F(EventSchedulerTest, TickFreesChainedZeroWhenEntriesInOneTick) {
    es_.Schedule(60, 1, 0);
    es_.Schedule(62, 1, 0);
    es_.Schedule(64, 1, 0);
    ASSERT_EQ(es_.size(), 3);
    es_.Tick();
    EXPECT_EQ(es_.size(), 0) << "all when==0 entries must free in one Tick";
    EXPECT_EQ(es_.root(), 0);
}

// Tick decrements EVERY pending entry (the walk covers the whole list), not
// just the head.
TEST_F(EventSchedulerTest, TickDecrementsAllEntriesInTheList) {
    es_.Schedule(60, 1, 10);
    es_.Schedule(62, 1, 8);
    es_.Schedule(64, 1, 6);
    es_.Tick();
    const auto list = ListContents(es_);
    ASSERT_EQ(list.size(), 3u);
    EXPECT_EQ(list[0].note, 64); EXPECT_EQ(list[0].when, 5);
    EXPECT_EQ(list[1].note, 62); EXPECT_EQ(list[1].when, 7);
    EXPECT_EQ(list[2].note, 60); EXPECT_EQ(list[2].when, 9);
}

// Remove(note, velocity) tags every matching entry as a zombie (kZombieSlot,
// 0xfe): it stays in the list and in size() (Remove does not decrement size),
// but the free-slot scan in Schedule skips 0xfe — the slot is not reused until
// Tick frees it when its delay expires.
TEST_F(EventSchedulerTest, RemoveTagsMatchingEntryAsZombie) {
    es_.Schedule(60, 1, 10);
    es_.Schedule(62, 1, 20);
    es_.Schedule(64, 1, 30);
    const uint8_t found = es_.Remove(62, 1);
    EXPECT_EQ(found, 1);
    // size unchanged: the zombie is still in the list.
    EXPECT_EQ(es_.size(), 3);
    const auto list = ListContents(es_);
    ASSERT_EQ(list.size(), 3u);
    EXPECT_EQ(list[1].note, kZombieSlot) << "middle entry must be zombie-tagged";
    EXPECT_EQ(list[0].note, 60);
    EXPECT_EQ(list[2].note, 64);
    // Zombie entries keep their `when` — Tick still frees them on schedule
    // (an entry scheduled with when=k is freed on tick k+1: the free loop
    // runs BEFORE the decrement).
    for (int t = 0; t < 11; t++) es_.Tick();
    EXPECT_EQ(es_.size(), 2) << "A(when=10) and the zombie(when=20) freed";
    for (int t = 0; t < 20; t++) es_.Tick();
    EXPECT_EQ(es_.size(), 0);
}

// Remove matches on (note AND velocity): a wrong velocity finds nothing and
// returns 0. Removing an already-zombified entry also finds nothing (zombie
// note 0xfe != the original note).
TEST_F(EventSchedulerTest, RemoveRequiresNoteAndVelocityMatch) {
    es_.Schedule(60, 7, 10);
    EXPECT_EQ(es_.Remove(60, 8), 0) << "velocity mismatch: no removal";
    EXPECT_EQ(es_.size(), 1);
    EXPECT_EQ(es_.Remove(61, 7), 0) << "note mismatch: no removal";
    ASSERT_EQ(es_.Remove(60, 7), 1);
    EXPECT_EQ(es_.Remove(60, 7), 0) << "already zombie: no second match";
}

// THE zombie-protocol lock: after Remove, the zombie slot is NOT handed out by
// a later Schedule (the free-slot scan matches note==kFreeSlot only), so a
// cancelled event's storage cannot be silently overwritten while it is still
// linked — that would corrupt the list exactly the way a dropped/stuck
// arpeggiator note sounds.
TEST_F(EventSchedulerTest, ZombieSlotIsNotReusedByLaterSchedule) {
    es_.Schedule(60, 1, 10);   // slot 1
    es_.Schedule(62, 1, 20);   // slot 2 (tail)
    ASSERT_EQ(es_.Remove(62, 1), 1);
    const uint8_t zombieSlot = 2;

    es_.Schedule(70, 1, 5);    // must take slot 3, NOT the zombie's slot 2
    ASSERT_EQ(es_.size(), 3);
    EXPECT_EQ(es_.entry(zombieSlot).note, kZombieSlot)
        << "zombie slot was reused by Schedule";
    EXPECT_EQ(es_.entry(zombieSlot).when, 20)  // zombie's fields are intact
        << "zombie entry was overwritten";
    // New note landed in a fresh slot and became the (earliest) head.
    EXPECT_EQ(es_.entry(es_.root()).note, 70);
    EXPECT_EQ(es_.entry(es_.root()).when, 5);
    // And the whole list still walks + drains cleanly.
    for (int t = 0; t < 30; t++) es_.Tick();
    EXPECT_EQ(es_.size(), 0);
    EXPECT_EQ(es_.root(), 0);
}

// overflow() is a soft watermark: true once size() >= kEventSchedulerSize - 8
// (82 of 90). It flips exactly at 82 entries.
TEST_F(EventSchedulerTest, OverflowFlagFlipsAtEightyTwoEntries) {
    for (int i = 0; i < 81; i++) es_.Schedule(20 + i, 1, 30);
    EXPECT_EQ(es_.size(), 81);
    EXPECT_FALSE(es_.overflow()) << "81 entries: below the 82 watermark";
    es_.Schedule(20 + 81, 1, 30);
    EXPECT_EQ(es_.size(), 82);
    EXPECT_TRUE(es_.overflow()) << "82 entries: at the watermark";
}

// Full queue: with all 89 entry slots (indices 1..89) live, Schedule is a
// SILENT no-op — size stays, nothing corrupts. (The spec'd "error handling:
// silent drop".) overflow() stays true throughout.
TEST_F(EventSchedulerTest, FullQueueScheduleIsSilentNoOp) {
    for (int i = 0; i < kEventSchedulerSize - 1; i++) {  // 89 entries
        es_.Schedule(20 + i, 1, 100 + i);
    }
    ASSERT_EQ(es_.size(), kEventSchedulerSize - 1);
    ASSERT_TRUE(es_.overflow());
    const auto before = ListContents(es_);
    es_.Schedule(200, 1, 1);  // no free slot -> silent drop
    EXPECT_EQ(es_.size(), kEventSchedulerSize - 1) << "queue full: size must not grow";
    const auto after = ListContents(es_);
    ASSERT_EQ(after.size(), before.size());
    for (size_t i = 0; i < before.size(); i++) {
        EXPECT_EQ(after[i].note, before[i].note) << "list corrupted by full Schedule";
        EXPECT_EQ(after[i].velocity, before[i].velocity) << "velocity corrupted";
        EXPECT_EQ(after[i].when, before[i].when) << "when corrupted";
    }
}

// Remove's MULTI-MATCH loop (event_scheduler.cpp:47-52): a single Remove
// pass zombifies EVERY entry matching (note AND velocity) and returns the
// count — the arp/sequencer callers rely on this to kill all pending copies
// of a retriggered note in one call.
TEST_F(EventSchedulerTest, RemoveTagsAllDuplicatesAsZombiesAndCountsThem) {
    es_.Schedule(60, 1, 5);
    es_.Schedule(64, 1, 10);
    es_.Schedule(60, 1, 15);  // duplicate (note, velocity) — must match too
    es_.Schedule(60, 2, 20);  // same note, different velocity — must survive
    ASSERT_EQ(es_.size(), 4);

    EXPECT_EQ(es_.Remove(60, 1), 2) << "both (60, vel 1) entries zombified";
    EXPECT_EQ(es_.size(), 4) << "zombies stay linked until Tick frees them";

    const auto list = ListContents(es_);
    int zombies = 0, survivors = 0;
    for (const auto& e : list) {
        if (e.note == kZombieSlot) zombies++;
        if (e.note == 60 && e.velocity == 2) survivors++;
    }
    EXPECT_EQ(zombies, 2);
    EXPECT_EQ(survivors, 1) << "the different-velocity duplicate survives";
    EXPECT_EQ(es_.Remove(60, 1), 0) << "second pass: entries are already zombies";
}

// note==0xff (== kFreeSlot) sentinel semantics, SAFE half only: a 0xff note is
// stored, counted in size(), and lives/dies through Tick exactly like any
// other note. (The unsafe half — the allocator's free-slot scan treating this
// LIVE entry as free, whose reuse self-loops the list and hangs Tick — is
// characterized in the file header, not driven: driving it would hang the
// suite.)
TEST_F(EventSchedulerTest, Note0xffIsStoredAndFreesNormally) {
    es_.Schedule(0xff, 7, 9);
    EXPECT_EQ(es_.size(), 1) << "a 0xff note still counts as scheduled";
    EXPECT_EQ(es_.entry(es_.root()).note, 0xff);
    EXPECT_EQ(es_.entry(es_.root()).when, 9);
    for (int t = 0; t < 9; t++) es_.Tick();
    EXPECT_EQ(es_.size(), 1);
    es_.Tick();  // 10th tick frees it
    EXPECT_EQ(es_.size(), 0);
    EXPECT_EQ(es_.root(), 0);
    // Remove can still zombie-tag it (note field matches), which UN-collides
    // the slot from kFreeSlot — the safe recovery path.
    es_.Schedule(0xff, 7, 9);
    EXPECT_EQ(es_.Remove(0xff, 7), 1);
    EXPECT_EQ(es_.entry(es_.root()).note, kZombieSlot);
}

// The 4-arg Schedule stores the tag; the 3-arg overload defaults tag to 0.
TEST_F(EventSchedulerTest, ScheduleStoresTagAndThreeArgOverloadDefaultsZero) {
    es_.Schedule(60, 1, 5, 42);
    EXPECT_EQ(es_.entry(es_.root()).tag, 42);
    es_.Schedule(62, 1, 6);
    const auto list = ListContents(es_);
    ASSERT_EQ(list.size(), 2u);
    // Entry order is by `when`; find the tag-0 entry via a fresh walk.
    uint8_t cur = es_.root();
    while (cur != 0 && es_.entry(cur).note != 62) cur = es_.entry(cur).next;
    ASSERT_NE(cur, 0);
    EXPECT_EQ(es_.entry(cur).tag, 0);
}

}  // namespace
