//
// Created by Spencer Martin on 4/10/25.
//
#include <../include/timing/timing.h>
#include <timing/Clock.h>
#include <arch.h>
#include <core/atomic.h>
#include <core/ds/LinkedList.h>
#include <core/ds/Trees.h>

namespace kernel::timing {
    struct CallbackWithHandle {
        TimerEventCallback callback;
        QueuedEventHandle handle;
    };

    struct QueuedTimerEvent {
        uint64_t expirationTime;
        QueuedTimerEvent* left;
        QueuedTimerEvent* right;
        QueuedTimerEvent* parent;
        bool isRed;

        LinkedList<CallbackWithHandle> callbacks;

        struct AugmentedData{
            QueuedTimerEvent* nextEvent;
            uint64_t earliestExpirationTime;
            uint64_t latestExpirationTime;

            bool operator==(const AugmentedData& other) const {
                return (nextEvent == other.nextEvent) && (earliestExpirationTime == other.earliestExpirationTime) && (latestExpirationTime == other.latestExpirationTime);
            }
        };

        AugmentedData augmentedData{};

        //bool operator<(const QueuedTimerEvent& other) const {return expirationTime < other.expirationTime;}

        bool operator==(const QueuedTimerEvent& other) const {return this == &other;}

        QueuedTimerEvent(CallbackWithHandle&& cb, const uint64_t et) : expirationTime(et) {
            callbacks.pushBack(move(cb));
            left = nullptr;
            right = nullptr;
            parent = nullptr;
            isRed = false;
            augmentedData = {this, expirationTime, expirationTime};
        }
    };

    struct QueuedEventInfoExtractor {
        static QueuedTimerEvent*& left(QueuedTimerEvent& event){return event.left;}
        static QueuedTimerEvent*& right(QueuedTimerEvent& event){return event.right;}
        static QueuedTimerEvent*& parent(QueuedTimerEvent& event){return event.parent;}
        static uint64_t& data(QueuedTimerEvent& event) {return event.expirationTime;}
        static QueuedTimerEvent* const& left(const QueuedTimerEvent& event){return event.left;}
        static QueuedTimerEvent* const& right(const QueuedTimerEvent& event){return event.right;}
        static QueuedTimerEvent* const& parent(const QueuedTimerEvent& event){return event.parent;}
        static const uint64_t& data(const QueuedTimerEvent& event) {return event.expirationTime;}
        static bool isRed(QueuedTimerEvent& event){return event.isRed;}
        static void setRed(QueuedTimerEvent& event, bool red){event.isRed = red;}
        static QueuedTimerEvent::AugmentedData& augmentedData(QueuedTimerEvent& event){return event.augmentedData;}
        static QueuedTimerEvent::AugmentedData recomputeAugmentedData(const QueuedTimerEvent& event, const QueuedTimerEvent* left, const QueuedTimerEvent* right) {
            uint64_t earliestExpirationTime = event.expirationTime;
            uint64_t latestExpirationTime = event.expirationTime;
            auto* nextEvent = const_cast<QueuedTimerEvent *>(&event);
            if (left) {
                nextEvent = left->augmentedData.nextEvent;
                earliestExpirationTime = left->expirationTime;
            }
            if (right) latestExpirationTime = right->expirationTime;
            return {nextEvent, earliestExpirationTime, latestExpirationTime};
        }
    };

    void dispatchTimerEvent();

    class TimerQueue {
        friend void dispatchTimerEvent();

        struct EventIDInfo {
            uint64_t id;
            QueuedTimerEvent* event;

            bool operator==(const EventIDInfo& other) const {return id == other.id;}
            bool operator<(const EventIDInfo& other) const {
                return id < other.id;
            }
        };

        IntrusiveRedBlackTree<QueuedTimerEvent, QueuedEventInfoExtractor> timerQueue;
        RedBlackTree<EventIDInfo> idToEventMap;
        uint64_t globalCounter;

        void removeIDFromEventMap(const uint64_t id) {
            //EventIDInfo::== ignores the event pointer when checking for equality, so while this is a bit of a hack
            //it does work
            idToEventMap.erase({id, nullptr});
        }

        QueuedTimerEvent* findQueuedEventInSubtreeFromID(const uint64_t id, const RedBlackTree<EventIDInfo>::Node* node) {
            if (node == nullptr) return nullptr;
            if (node->data.id == id) return node->data.event;
            return findQueuedEventInSubtreeFromID(id, id < node->data.id ? node->left : node->right);
        }

        QueuedTimerEvent* findQueuedEventFromID(const uint64_t id) {
            return findQueuedEventInSubtreeFromID(id, idToEventMap.getRoot());
        }

        QueuedTimerEvent* findCoalescableEvent(uint64_t earlyTime, uint64_t lateTime) {
            return searchSubtree(timerQueue.getRoot(), earlyTime, lateTime);
        }

        QueuedTimerEvent* searchSubtree(QueuedTimerEvent* node, uint64_t early, uint64_t late) {
            if (!node) return nullptr;

            // Check if this subtree can possibly contain a match
            if (node->augmentedData.latestExpirationTime < early) return nullptr;  // Too early
            if (node->augmentedData.earliestExpirationTime > late) return nullptr;  // Too late

            // Check if current node itself is coalescable
            if (early <= node->expirationTime && node->expirationTime <= late) {
                return node;
            }

            // Recursively search children
            // Prefer left (earlier timers) for better coalescing
            if (QueuedTimerEvent* result = searchSubtree(node->left, early, late)) {
                return result;
            }

            return searchSubtree(node->right, early, late);
        }

        EventSource& es;
    public:
        QueuedEventHandle enqueueTimerEvent(TimerEventCallback&& cb, uint64_t expirationTime, uint64_t lateTolerance, uint64_t earlyTolerance) {
            arch::InterruptDisabler id;
            uint64_t earlyTime = expirationTime - earlyTolerance;
            uint64_t lateTime = expirationTime + lateTolerance;

            if (monoTimens() >= earlyTime) {
                cb();
                return EXPIRED_EVENT;
            }
            QueuedEventHandle handle;
            if (const auto coalescable = findCoalescableEvent(earlyTime, lateTime)) {
                handle = {globalCounter++};
                CallbackWithHandle cbWithHandle{move(cb), handle};
                coalescable->callbacks.pushBack(move(cbWithHandle));
                idToEventMap.insert({handle.id, coalescable});
            }
            else {
                handle = {globalCounter++};
                CallbackWithHandle cbWithHandle{move(cb), handle};
                const auto event = new QueuedTimerEvent(move(cbWithHandle), expirationTime);
                timerQueue.insert(event);
                idToEventMap.insert({handle.id, event});
            }
            id.release();
            flushExpiredEvents();
            return handle;
        }

        bool cancelTimerEvent(QueuedEventHandle handle) {
            arch::InterruptDisabler id;
            auto* event = findQueuedEventFromID(handle.id);
            if (event == nullptr) return false;
            if (event->callbacks.headNode() == event -> callbacks.tailNode()) {
                if (event->callbacks.head()->handle.id != handle.id) {
                    return false;
                }
                timerQueue.erase(event);
                removeIDFromEventMap(handle.id);
                id.release();
                delete event;
                //If there's only one event scheduled for the time, then we may need to reprogram the event source
                flushExpiredEvents();
                return true;
            }
            auto* currNode = event->callbacks.headNode();
            while (currNode != nullptr) {
                if (currNode->data.handle.id == handle.id) {
                    event -> callbacks.remove(currNode);
                    removeIDFromEventMap(handle.id);
                    return true;
                }
                currNode = currNode -> next;
            }
            return false;
        }

        void flushExpiredEvents() {
            Vector<TimerEventCallback> callbacks;
            while (true) {
                {
                    arch::InterruptDisabler id;
                    while (auto* root = timerQueue.getRoot()) {
                        auto* event = root->augmentedData.nextEvent;
                        if (monoTimens() < event->expirationTime) break;

                        for (auto& cb : event->callbacks) {
                            callbacks.push(move(cb.callback));
                            removeIDFromEventMap(cb.handle.id);
                        }

                        timerQueue.erase(event);
                        delete event;
                    }
                }
                for (auto& cb : callbacks) {
                    cb();
                }
                callbacks.clear();
                {
                    arch::InterruptDisabler id;
                    if (auto* root = timerQueue.getRoot()) {
                        auto*& event = root->augmentedData.nextEvent;
                        uint64_t now = monoTimens();
                        if (now >= event->expirationTime) {
                            continue;
                        }
                        const auto deltaTicks = es.calibrationData().nanosToTicks(event->expirationTime - now);
                        es.armOneshot(min(deltaTicks, es.maxPeriod()));
                        return;
                    }
                    else {
                        es.disarm();
                        return;
                    }
                }
            }
        }

        explicit TimerQueue(EventSource& eventSource) : es(eventSource) {
            assert(es.supportsOneshot(), "We don't support periodic timers for this system right now");
        }

        TimerQueue() : TimerQueue(getEventSource()){}
    };

    TimerQueue* localTimerQueues;

    void initTimerQueues() {
        localTimerQueues = new TimerQueue[arch::processorCount()];
        getEventSource().registerCallback(dispatchTimerEvent);
    }

    TimerQueue& localQueue() {
        return localTimerQueues[arch::getCurrentProcessorID()];
    }

    void dispatchTimerEvent() {
        localQueue().flushExpiredEvents();
    }

    QueuedEventHandle enqueueEvent(TimerEventCallback&& cb, uint64_t preferredDelayMs, uint64_t lateTolerance, uint64_t earlyTolerance) {
        arch::InterruptDisabler id;
        return localQueue().enqueueTimerEvent(move(cb), monoTimens() + preferredDelayMs * 1'000'000,
            lateTolerance * 1'000'000, earlyTolerance * 1'000'000);
    }

    bool cancelEvent(QueuedEventHandle handle) {
        arch::InterruptDisabler id;
        return localQueue().cancelTimerEvent(handle);
    }
}