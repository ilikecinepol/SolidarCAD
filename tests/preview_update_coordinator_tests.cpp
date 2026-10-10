#include "TestAssertions.h"

#include "ui/PreviewUpdateCoordinator.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QObject>

#include <cstdlib>
#include <iostream>
#include <memory>

int main(int argc, char** argv) {
  QCoreApplication application(argc, argv);

  // A burst is last-write-wins and an explicit boundary flush is synchronous.
  QObject owner;
  solidar::PreviewUpdateCoordinator coordinator(&owner, 60'000);
  int published = -1;
  for (int value = 0; value < 100; ++value) {
    static_cast<void>(coordinator.request(
        [&coordinator, &published, value](auto token) {
          if (coordinator.claimPublication(token)) published = value;
        }));
  }
  CHECK(coordinator.hasPending());
  coordinator.flush();
  CHECK(published == 99);
  CHECK(!coordinator.hasPending());
  CHECK(coordinator.counters().requests == 100);
  CHECK(coordinator.counters().replacements == 99);
  CHECK(coordinator.counters().executions == 1);
  CHECK(coordinator.counters().publications == 1);
  CHECK(coordinator.counters().flushes == 1);
  CHECK(coordinator.counters().cadenceStarts == 1);

  // Sustained input faster than the cadence still executes periodically. A
  // trailing debounce would starve this stream and publish only after it ends.
  QObject cadenceOwner;
  solidar::PreviewUpdateCoordinator cadence(&cadenceOwner, 60'000);
  int cadenceValue = -1;
  for (int frame = 0; frame < 6; ++frame) {
    for (int sample = 0; sample < 20; ++sample) {
      const int value = frame * 20 + sample;
      static_cast<void>(cadence.request(
          [&cadence, &cadenceValue, value](auto token) {
            if (cadence.claimPublication(token)) cadenceValue = value;
          }));
    }
    cadence.dispatchCadenceForTests();
    CHECK(cadenceValue == frame * 20 + 19);
  }
  CHECK(cadence.counters().requests == 120);
  CHECK(cadence.counters().executions == 6);
  CHECK(cadence.counters().cadenceStarts == 6);
  CHECK(cadence.counters().replacements == 114);
  CHECK(cadenceValue == 119);
  CHECK(!cadence.hasPending());

  // Invalidation cancels delayed work and advances the generation.
  const auto oldGeneration = coordinator.generation();
  static_cast<void>(coordinator.request(
      [&published](auto) { published = 1000; }));
  coordinator.invalidate();
  QCoreApplication::processEvents(QEventLoop::AllEvents);
  CHECK(published == 99);
  CHECK(coordinator.generation() == oldGeneration + 1);
  CHECK(coordinator.counters().cancellations >= 100);

  // A callback may synchronously enqueue a newer request. Its old token loses
  // publication rights even though the expensive callback already started.
  int firstPublications = 0;
  int secondPublications = 0;
  static_cast<void>(coordinator.request(
      [&coordinator, &firstPublications, &secondPublications](auto first) {
        static_cast<void>(coordinator.request(
            [&coordinator, &secondPublications](auto second) {
              if (coordinator.claimPublication(second)) ++secondPublications;
            }));
        if (coordinator.claimPublication(first)) ++firstPublications;
      }));
  coordinator.flush();
  CHECK(firstPublications == 0);
  CHECK(coordinator.hasPending());
  coordinator.flush();
  CHECK(secondPublications == 1);
  CHECK(coordinator.counters().staleRejections >= 1);

  // QObject ownership must cancel the timer; no callback survives teardown.
  int afterDestruction = 0;
  auto transientOwner = std::make_unique<QObject>();
  auto* transient = new solidar::PreviewUpdateCoordinator(
      transientOwner.get(), 0);
  static_cast<void>(transient->request(
      [&afterDestruction](auto) { ++afterDestruction; }));
  transientOwner.reset();
  QCoreApplication::processEvents(QEventLoop::AllEvents);
  CHECK(afterDestruction == 0);

  return EXIT_SUCCESS;
}
