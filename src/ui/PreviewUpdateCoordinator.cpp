#include "ui/PreviewUpdateCoordinator.h"

#include <QThread>
#include <QTimer>

#include <algorithm>
#include <utility>

namespace solidar {

PreviewUpdateCoordinator::PreviewUpdateCoordinator(QObject* owner,
                                                     int debounceMs)
    : QObject(owner), timer_(new QTimer(this)) {
  timer_->setSingleShot(true);
  timer_->setInterval(debounceMs);
  connect(timer_, &QTimer::timeout, this,
          [this] { executePending(); });
}

PreviewUpdateCoordinator::Token PreviewUpdateCoordinator::request(Work work) {
  const Token token{generation_, nextSequence_++};
  ++counters_.requests;
  if (QThread::currentThread() != thread() || !work) return token;
  latestRequestedSequence_ = token.sequence;
  if (pending_) {
    ++counters_.replacements;
    ++counters_.cancellations;
  }
  pending_ = std::move(work);
  pendingToken_ = token;
  // Frame throttle, not a trailing debounce: a continuous stream must publish
  // the latest request once per cadence instead of starving until input stops.
  if (!timer_->isActive()) {
    timer_->start();
    ++counters_.cadenceStarts;
  }
  return token;
}

void PreviewUpdateCoordinator::flush() {
  if (QThread::currentThread() != thread()) return;
  ++counters_.flushes;
  timer_->stop();
  executePending();
}

void PreviewUpdateCoordinator::invalidate() noexcept {
  timer_->stop();
  if (pending_) ++counters_.cancellations;
  pending_ = {};
  pendingToken_ = {};
  latestRequestedSequence_ = 0;
  publishedSequence_ = 0;
  ++generation_;
  ++counters_.invalidations;
}

bool PreviewUpdateCoordinator::isCurrent(Token token) const noexcept {
  return token.generation == generation_ && token.sequence != 0 &&
         token.sequence == latestRequestedSequence_;
}

bool PreviewUpdateCoordinator::claimPublication(Token token) noexcept {
  if (!isCurrent(token) || publishedSequence_ == token.sequence) {
    ++counters_.staleRejections;
    return false;
  }
  publishedSequence_ = token.sequence;
  ++counters_.publications;
  return true;
}

bool PreviewUpdateCoordinator::hasPending() const noexcept {
  return static_cast<bool>(pending_);
}

std::uint64_t PreviewUpdateCoordinator::generation() const noexcept {
  return generation_;
}

const PreviewUpdateCoordinator::Counters&
PreviewUpdateCoordinator::counters() const noexcept {
  return counters_;
}

void PreviewUpdateCoordinator::setCadenceIntervalForTests(int milliseconds) {
  timer_->setInterval(std::max(0, milliseconds));
}

void PreviewUpdateCoordinator::dispatchCadenceForTests() {
  if (QThread::currentThread() != thread()) return;
  timer_->stop();
  executePending();
}

void PreviewUpdateCoordinator::executePending() {
  if (!pending_) return;
  Work work = std::move(pending_);
  pending_ = {};
  const Token token = pendingToken_;
  pendingToken_ = {};
  if (!isCurrent(token)) {
    ++counters_.staleRejections;
    return;
  }
  ++counters_.executions;
  work(token);
}

}  // namespace solidar
