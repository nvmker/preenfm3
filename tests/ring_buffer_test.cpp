// Host-side coverage for lib/Inc/RingBuffer.h — the SPSC ring used across
// the firmware (MIDI byte streams, sequencer/decoder async-action queues).
//
// 8.1 (C15) suite: pins the insertChecked() contract. insert() on a FULL
// ring is not a checked append — it advances the tail ONTO the head, so
// getCount() reads 0 and one overflow insert destroys EVERY pending entry.
// insertChecked() must refuse (false) and leave the ring completely
// untouched. The last test documents the dangerous insert() behavior on
// purpose: it is why insertChecked() exists, and why insert() is kept only
// for callers with a proven reservation (hasRoomFor / multi-element atomic
// reservations — see the SPSC caveat in RingBuffer.h).
#include "RingBuffer.h"

#include "gtest/gtest.h"

TEST(RingBufferTest, InsertCheckedFillsToCapacityThenRejects) {
    // RingBuffer<int, 4> sacrifices one slot: usable capacity is 3.
    RingBuffer<int, 4> ring;
    ASSERT_EQ(ring.getCount(), 0);

    EXPECT_TRUE(ring.insertChecked(1));
    EXPECT_TRUE(ring.insertChecked(2));
    EXPECT_TRUE(ring.insertChecked(3));
    EXPECT_EQ(ring.getCount(), 3);
    EXPECT_TRUE(ring.isFull());

    // 4th element: refused, and the ring state is untouched.
    EXPECT_FALSE(ring.insertChecked(4));
    EXPECT_EQ(ring.getCount(), 3) << "a refused insert must not change the count";
    EXPECT_TRUE(ring.isFull());

    // FIFO order intact: the three inserted elements drain in order, and
    // the refused element is NOT in the queue.
    EXPECT_EQ(ring.remove(), 1);
    EXPECT_EQ(ring.remove(), 2);
    EXPECT_EQ(ring.remove(), 3);
    EXPECT_EQ(ring.getCount(), 0);
}

TEST(RingBufferTest, InsertCheckedRecoversAfterConsumerFreesSpace) {
    // After a refused insert, the producer can insert again once the
    // consumer frees a slot (the SPSC recovery path).
    RingBuffer<int, 4> ring;
    EXPECT_TRUE(ring.insertChecked(1));
    EXPECT_TRUE(ring.insertChecked(2));
    EXPECT_TRUE(ring.insertChecked(3));
    EXPECT_FALSE(ring.insertChecked(4));
    EXPECT_EQ(ring.remove(), 1);
    EXPECT_TRUE(ring.insertChecked(4));
    EXPECT_EQ(ring.getCount(), 3);
    EXPECT_EQ(ring.remove(), 2);
    EXPECT_EQ(ring.remove(), 3);
    EXPECT_EQ(ring.remove(), 4);
}

TEST(RingBufferTest, PlainInsertOnFullRingEmptiesIt) {
    // DANGER DOCUMENTED (C15): insert() on a full ring writes the element
    // and advances the tail onto the head — getCount() then reads 0, so one
    // overflow insert destroys EVERY pending entry (they become unreadable
    // garbage slots). This pins the hazard insertChecked() exists to close;
    // if insert() is ever changed to a checked append, this test flips and
    // must be updated together with the hasRoomFor() reservation callers.
    RingBuffer<int, 4> ring;
    ring.insert(1);
    ring.insert(2);
    ring.insert(3);
    ASSERT_TRUE(ring.isFull());

    ring.insert(4);  // the overflow insert

    EXPECT_EQ(ring.getCount(), 0)
        << "overflow insert() advanced the tail onto the head — all pending "
           "entries destroyed (the C15 hazard insertChecked closes)";
}
