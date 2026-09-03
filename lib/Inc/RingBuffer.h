/*
 * Copyright 2013 Xavier Hosxe
 *
 * Author: Xavier Hosxe (xavier . hosxe (at) gmail . com)
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#ifndef RINGBUFFER_H_
#define RINGBUFFER_H_


template <typename T, int size>
class RingBuffer {
public:
	RingBuffer() {
	    clear();
	}

	~RingBuffer() {
	};

	void clear() {
        this->head = 0;
        this->tail = 0;
	}

    // Runtime SPSC discard for the consumer. Linearizes at the tail read:
    // entries published before it are discarded, while a producer insert
    // that completes afterwards remains available. Unlike clear(), this
    // never writes the producer-owned tail.
    void discardAllFromConsumer() {
        this->head = this->tail;
    }

    // B1: producer-side SPSC discard, mirroring discardAllFromConsumer()
    // above. Linearizes at the tail write: unread entries are discarded,
    // while a consumer remove() that completes afterwards still sees a
    // coherent (empty) queue. Unlike clear(), this never writes the
    // consumer-owned head — safe to call from the producer context (main
    // loop) while SysTick drains the queue.
    //
    // B1/Copilot: a plain `tail = head` still re-reads the CONSUMER-owned
    // head — a SysTick remove() landing between the load and the store
    // writes a STALE head into tail, and getCount() then reports size-1 (a
    // "full" queue of stale slots). Re-stabilize until the loaded head is
    // still current; head only advances (at most once per tic), so the loop
    // converges as soon as the consumer pauses one iteration.
    void discardAllFromProducer() {
        for (;;) {
            int h = this->head;
            this->tail = h;
            if (this->head == h) {
                break;
            }
        }
    }

	void insert(T element) {
	    this->buf[this->tail] = element;
	    this->tail = (this->tail == size-1) ? 0 : this->tail + 1;
	}

	T remove() {
	    T element = this->buf[this->head];
	    this->head = (this->head == size-1) ? 0 : this->head + 1;
	    return element;
	}

	bool isFull() {
	   return (this->tail + 1 == this->head) ||
	        (this->tail == (size-1) && this->head == 0);
	}

	int getCount() {
	    int count = this->tail - this->head;
	    if (this->tail < this->head) {
	    	count += size;
	    }
	    return count;
	}

	// 6.9: can n more elements be inserted without overwriting unread data?
	// The ring sacrifices one slot (isFull() at count == size-1), so usable
	// capacity is size-1 and room-for-n means count + n <= size-1.
	// NOTE: like isFull(), this is a QUERY — the guarantee holds only if the
	// caller performs the inserts without yielding (the firmware's
	// cooperative main loop does; the USART ISR only drains). insert()
	// itself still overwrites unconditionally.
	bool hasRoomFor(int n) {
	    return getCount() + n < size;
	}

	void appendBlock(T* block, int number) {
		for (int k=0; k<number ; k++) {
			insert(block[k]);
		}
	}

private:
    volatile int head;
    volatile int tail;
    T buf[size+1];

};

#endif /* RINGBUFFER_H_ */
