// Spinlock that also disables interrupts while held, so code holding it can't
// be preempted or re-entered from an interrupt handler on this CPU.
#pragma once

#include "arch/x86_64/cpu.hpp"

class SpinLock {
 public:
  void lock() {
    bool irq = arch::irq_save();
    while (__atomic_exchange_n(&locked_, true, __ATOMIC_ACQUIRE)) arch::pause();
    irq_was_on_ = irq;
  }
  void unlock() {
    bool irq = irq_was_on_;
    __atomic_store_n(&locked_, false, __ATOMIC_RELEASE);
    arch::irq_restore(irq);
  }
  bool held() const { return __atomic_load_n(&locked_, __ATOMIC_RELAXED); }

 private:
  bool locked_ = false;
  bool irq_was_on_ = false;
};

class LockGuard {
 public:
  explicit LockGuard(SpinLock& l) : l_(l) { l_.lock(); }
  ~LockGuard() { l_.unlock(); }
  LockGuard(const LockGuard&) = delete;
  LockGuard& operator=(const LockGuard&) = delete;

 private:
  SpinLock& l_;
};
