/*
 * Copyright (c) 2013-2014 ARM Limited
 * All rights reserved
 *
 * The license below extends only to copyright in the software and shall
 * not be construed as granting a license to any other intellectual
 * property including but not limited to intellectual property relating
 * to a hardware implementation of the functionality of the software
 * licensed hereunder.  You may use the software subject to the license
 * terms below provided that you ensure that this notice is replicated
 * unmodified and in its entirety in all distributions of the software,
 * modified or unmodified, in source code or in binary form.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/**
 * @file
 *
 * Classes for buffer, queue and FIFO behaviour.
 */

#error DO NOT INCLUDE ME
#pragma once
#include <iostream>
#include <queue>
#include <sstream>
#include <string>

#include "base/logging.hh"
#include "base/named.hh"
#include "base/types.hh"
#include "cpu/activity.hh"
#include "cpu/timebuf.hh"

namespace gem5 {
namespace cva6 {



/** Allow a template using ReportTraits to call report... functions of
 *  ReportIF-bearing elements themselves */
template <typename ElemType> /* ElemType should implement ReportIF */
class ReportTraitsAdaptor
{
  public:
    static void
    reportData(std::ostream &os, const ElemType &elem)
    { elem.reportData(os); }
};

/** A similar adaptor but for elements held by pointer
 *  ElemType should implement ReportIF */
template <typename PtrType>
class ReportTraitsPtrAdaptor
{
  public:
    static void
    reportData(std::ostream &os, const PtrType &elem)
    { elem->reportData(os); }
};

/** ... BubbleTraits are trait classes to add BubbleIF interface
 *  functionality to templates which process elements which don't necessarily
 *  implement BubbleIF themselves */

/** Default behaviour, no bubbles */
template <typename ElemType>
class NoBubbleTraits
{
  public:
    static bool isBubble(const ElemType &) { return false; }
    static ElemType
    bubble()
    {
        panic("bubble called but no bubble interface");
    }
};

/** Pass on call to the element */
template <typename ElemType>
class BubbleTraitsAdaptor
{
  public:
    static bool isBubble(const ElemType &elem)
    { return elem.isBubble(); }

    static ElemType bubble() { return ElemType::bubble(); }
};

/** Pass on call to the element where the element is a pointer */
template <typename PtrType, typename ElemType>
class BubbleTraitsPtrAdaptor
{
  public:
    static bool isBubble(const PtrType &elem)
    { return elem->isBubble(); }

    static PtrType bubble() { return ElemType::bubble(); }
};

/** TimeBuffer with Named interfaces */
template <typename ElemType,
    typename ReportTraits = ReportTraitsAdaptor<ElemType>,
    typename BubbleTraits = BubbleTraitsAdaptor<ElemType> >
class Cva6Buffer : public Named, public TimeBuffer<ElemType>
{
  protected:
    /** The range of elements that should appear in trace lines */
    int reportLeft, reportRight;

  public:
    Cva6Buffer(const std::string &name,
        const std::string &data_name,
        int num_past, int num_future,
        int report_left = -1, int report_right = -1) :
        Named(name), TimeBuffer<ElemType>(num_past, num_future),
        reportLeft(report_left), reportRight(report_right)
    { }

  public:
    /* Is this buffer full of only bubbles */
    bool
    empty() const
    {
        bool ret = true;

        for (int i = -this->past; i <= this->future; i++) {
            if (!BubbleTraits::isBubble((*this)[i]))
                ret = false;
        }

        return ret;
    }

    std::string dump() const
    {
        std::ostringstream data;
        int step = (reportLeft > reportRight ? -1 : 1);
        int end = reportRight + step;
        int i = reportLeft;

        while (i != end) {
            const ElemType &datum = (*this)[i];

            ReportTraits::reportData(data, datum);
            i += step;
            if (i != end)
                data << ',';
        }

        return data.str();
    }
};

/** Wraps a Cva6Buffer with Input/Output interfaces to ensure that units
 *  within the model can only see the right end of buffers between them. */
template <typename Data>
class Latch
{
  public:
    typedef Cva6Buffer<Data> Buffer;

  protected:
    /** Delays, in cycles, writing data into the latch and seeing it on the
     *  latched wires */
    Cycles delay;

    Buffer buffer;

  public:
    /** forward/backwardDelay specify the delay from input to output in each
     *  direction.  These arguments *must* be >= 1 */
    Latch(const std::string &name,
        const std::string &data_name,
        bool report_backwards = false) :
        delay(Cycles(1)),
        buffer(name, data_name, delay, 0, (report_backwards ? -delay : 0),
            (report_backwards ? 0 : -delay))
    { }

  public:
    /** Encapsulate wires on either input or output of the latch.
     *  forward/backward correspond to data direction relative to the
     *  pipeline.  Latched and Immediate specify delay for backward data.
     *  Immediate data is available to earlier stages *during* the cycle it
     *  is written */
    class Input
    {
      public:
        typename Buffer::wire inputWire;

      public:
        Input(typename Buffer::wire input_wire) :
            inputWire(input_wire)
        { }
    };

    class Output
    {
      public:
        typename Buffer::wire outputWire;

      public:
        Output(typename Buffer::wire output_wire) :
            outputWire(output_wire)
        { }
    };

    bool empty() const { return buffer.empty(); }

    /** An interface to just the input of the buffer */
    Input input() { return Input(buffer.getWire(0)); }

    /** An interface to just the output of the buffer */
    Output output() { return Output(buffer.getWire(-delay)); }

    void evaluate() { buffer.advance(); }

    std::string dump() { return buffer.dump(); }
};

/** A pipeline simulating class that will stall (not advance when advance()
 *  is called) if a non-bubble value lies at the far end of the pipeline.
 *  The user can clear the stall before calling advance to unstall the
 *  pipeline. */
template <typename ElemType,
    typename ReportTraits,
    typename BubbleTraits = BubbleTraitsAdaptor<ElemType> >
class SelfStallingPipeline : public Cva6Buffer<ElemType, ReportTraits>
{
  protected:
    /** Wire at the input end of the pipeline (for convenience) */
    typename TimeBuffer<ElemType>::wire pushWire;
    /** Wire at the output end of the pipeline (for convenience) */
    typename TimeBuffer<ElemType>::wire popWire;

  public:
    /** If true, advance will not advance the pipeline */
    bool stalled;

    /** The number of slots with non-bubbles in them */
    unsigned int occupancy;

  public:
    SelfStallingPipeline(const std::string &name,
        const std::string &data_name,
        unsigned depth) :
        Cva6Buffer<ElemType, ReportTraits>
            (name, data_name, depth, 0, -1, -depth),
        pushWire(this->getWire(0)),
        popWire(this->getWire(-depth)),
        stalled(false),
        occupancy(0)
    {
        assert(depth > 0);

        /* Write explicit bubbles to get around the case where the default
         *  constructor for the element type isn't good enough */
        for (unsigned i = 0; i <= depth; i++)
            (*this)[-i] = BubbleTraits::bubble();
    }

  public:
    /** Write an element to the back of the pipeline.  This doesn't cause
     *  the pipeline to advance until advance is called.  Pushing twice
     *  without advance-ing will just cause an overwrite of the last push's
     *  data. */
    void push(ElemType &elem)
    {
        assert(!alreadyPushed());
        *pushWire = elem;
        if (!BubbleTraits::isBubble(elem))
            occupancy++;
    }

    /** Peek at the end element of the pipe */
    ElemType &front() { return *popWire; }

    const ElemType &front() const { return *popWire; }

    /** Have we already pushed onto this pipe without advancing */
    bool alreadyPushed() { return !BubbleTraits::isBubble(*pushWire); }

    /** There's data (not a bubble) at the end of the pipe */
    bool isPopable() { return !BubbleTraits::isBubble(front()); }

    /** Try to advance the pipeline.  If we're stalled, don't advance.  If
     *  we're not stalled, advance then check to see if we become stalled
     *  (a non-bubble at the end of the pipe) */
    void
    advance()
    {
        bool data_at_end = isPopable();

        if (!stalled) {
            TimeBuffer<ElemType>::advance();
            /* If there was data at the end of the pipe that has now been
             *  advanced out of the pipe, we've lost data */
            if (data_at_end)
                occupancy--;
            /* Is there data at the end of the pipe now? */
            stalled = isPopable();
            /* Insert a bubble into the empty input slot to make sure that
             *  element is correct in the case where the default constructor
             *  for ElemType doesn't produce a bubble */
            ElemType bubble = BubbleTraits::bubble();
            *pushWire = bubble;
        }
    }


    /** Flush the pipeline. */
    void flush(){
      while (!Cva6Buffer<ElemType, ReportTraits>::empty()){
        advance();
        stalled = false;
      }
    }
};

/** Base class for space reservation requestable objects */
class Reservable
{
  public:
    /** Can a slot be reserved? */
    virtual bool canReserve() const = 0;

    /** Reserve a slot in whatever structure this is attached to */
    virtual void reserve() = 0;

    /** Free a reserved slot */
    virtual void freeReservation() = 0;

    virtual ~Reservable() {};
};

/** Wrapper for a queue type to act as a pipeline stage input queue.
 *  Handles capacity management, bubble value suppression and provides
 *  reporting.
 *
 *  In an ideal world, ElemType would be derived from ReportIF and BubbleIF,
 *  but here we use traits and allow the Adaptors ReportTraitsAdaptor and
 *  BubbleTraitsAdaptor to work on data which *does* directly implement
 *  those interfaces. */
template <typename ElemType,
    typename ReportTraits = ReportTraitsAdaptor<ElemType>,
    typename BubbleTraits = BubbleTraitsAdaptor<ElemType> >
class Queue : public Named, public Reservable
{
  private:
      std::deque<ElemType> queue;

    /** Number of slots currently reserved for future (reservation
     *  respecting) pushes */
    unsigned int numReservedSlots;

    /** Need this here as queues usually don't have a limited capacity */
    unsigned int capacity;

  public:
    Queue(const std::string &name, const std::string &data_name,
        unsigned int capacity_) :
        Named(name),
        numReservedSlots(0),
        capacity(capacity_) { }

  public:
    /** Push an element into the buffer if it isn't a bubble.  Bubbles are
     *  just discarded.  It is assummed that any push into a queue with
     *  reserved space intends to take that space */
    void
    push(ElemType &data)
    {
        if (!BubbleTraits::isBubble(data)) {
            freeReservation();
            queue.push_back(data);

            if (queue.size() > capacity) {
                warn("%s: No space to push data into queue of capacity"
                    " %u, pushing anyway\n", name(), capacity);
            }

        }
    }

    /** Clear all allocated space.  Be careful how this is used */
    void clearReservedSpace() { numReservedSlots = 0; }

    /** Clear a single reserved slot */
    void freeReservation()
    {
        if (numReservedSlots != 0)
            numReservedSlots--;
    }

    /** Reserve space in the queue for future pushes.  Enquiries about space
     *  in the queue using unreservedRemainingSpace will only tell about
     *  space which is not full and not reserved. */
    void
    reserve()
    {
        /* Check reservable space */
        if (unreservedRemainingSpace() == 0)
            warn("%s: No space is reservable in queue", name());

        numReservedSlots++;
    }

    bool canReserve() const { return unreservedRemainingSpace() != 0; }

    /** Number of slots available in an empty buffer */
    unsigned int totalSpace() const { return capacity; }

    /** Number of slots already occupied in this buffer */
    unsigned int occupiedSpace() const { return queue.size(); }

    /** Number of slots which are reserved. */
    unsigned int reservedSpace() const { return numReservedSlots; }

    /** Number of slots yet to fill in this buffer.  This doesn't include
     *  reservation. */
    unsigned int
    remainingSpace() const
    {
        int ret = capacity - queue.size();

        return (ret < 0 ? 0 : ret);
    }

    /** Like remainingSpace but does not count reserved spaces */
    unsigned int
    unreservedRemainingSpace() const
    {
        int ret = capacity - (queue.size() + numReservedSlots);

        return (ret < 0 ? 0 : ret);
    }

    /** Head value.  Like std::queue::front */
    ElemType &front() { return queue.front(); }

    const ElemType &front() const { return queue.front(); }

    /** Pop the head item.  Like std::queue::pop */
    void pop() { queue.pop_front(); }

    /** Is the queue empty? */
    bool empty() const { return queue.empty(); }
};

/** Like a Queue but with a restricted interface and a setTail function
 *  which, when the queue is empty, just takes a reference to the pushed
 *  item as the single element.  Calling pushTail will push that element
 *  onto the queue.
 *
 *  The purpose of this class is to allow the faster operation of queues of
 *  items which usually don't get deeper than one item and for which the copy
 *  associated with a push is expensive enough to want to avoid
 *
 *  The intended use case is the input buffer for pipeline stages, hence the
 *  class name */
template <typename ElemType,
    typename ReportTraits = ReportTraitsAdaptor<ElemType>,
    typename BubbleTraits = BubbleTraitsAdaptor<ElemType> >
class InputBuffer : public Reservable
{
  protected:
    /** Underlying queue */
    mutable Queue<ElemType, ReportTraits, BubbleTraits> queue;

    /** Pointer to the single element (if not NULL) */
    mutable ElemType *elementPtr;

  public:
    InputBuffer(const std::string &name, const std::string &data_name,
        unsigned int capacity_=1) :
        queue(name, data_name, capacity_),
        elementPtr(NULL)
    { }

  public:
    /** Set the tail of the queue, this is like push but needs
     *  to be followed by pushTail for the new tail to make its
     *  way into the queue proper */
    void
    setTail(ElemType &new_element)
    {
        assert(!elementPtr);
        if (!BubbleTraits::isBubble(new_element)) {
            if (queue.empty())
                elementPtr = &new_element;
            else
                queue.push(new_element);
        }
    }

    /** No single element or queue entries */
    bool empty() const { return !elementPtr && queue.empty(); }

    /** Return the element, or the front of the queue */
    const ElemType &front() const
    { return (elementPtr ? *elementPtr : queue.front()); }

    ElemType &front()
    { return (elementPtr ? *elementPtr : queue.front()); }

    /** Pop either the head, or if none, the head of the queue */
    void
    pop()
    {
        if (elementPtr) {
            /* A popped element was expected to be pushed into queue
             *  and so take a reserved space */
            elementPtr = NULL;
            queue.freeReservation();
        } else {
            queue.pop();
        }
    }

    /** Push the single element (if any) into the queue proper.  If the
     *  element's reference points to a transient object, remember to
     *  always do this before the end of that object's life */
    void
    pushTail() const
    {
        if (elementPtr)
            queue.push(*elementPtr);
        elementPtr = NULL;
    }

    /** Reservable interface, passed on to queue */
    bool canReserve() const { return queue.canReserve(); }
    void reserve() { queue.reserve(); }
    void freeReservation() { queue.freeReservation(); }

    /** Like remainingSpace but does not count reserved spaces */
    unsigned int
    unreservedRemainingSpace()
    {
        pushTail();
        return queue.unreservedRemainingSpace();
    }
};


} // namespace cva6
} // namespace gem5
