/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * Copyright (c) 2014-2024,  Regents of the University of California,
 *                           Arizona Board of Regents,
 *                           Colorado State University,
 *                           University Pierre & Marie Curie, Sorbonne University,
 *                           Washington University in St. Louis,
 *                           Beijing Institute of Technology,
 *                           The University of Memphis.
 *
 * This file is part of NFD (Named Data Networking Forwarding Daemon).
 * See AUTHORS.md for complete list of NFD authors and contributors.
 *
 * NFD is free software: you can redistribute it and/or modify it under the terms
 * of the GNU General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * NFD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 * PURPOSE.  See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * NFD, e.g., in COPYING.md file.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "fw/asf-strategy.hpp"

#include "tests/daemon/face/dummy-face.hpp"

#include "strategy-tester.hpp"
#include "topology-tester.hpp"

namespace nfd::tests {

using fw::AsfStrategy;
using fw::asf::FaceInfo;

// The tester is unused in this file, but it's used in various templated test suites.
using AsfStrategyTester = StrategyTester<AsfStrategy>;
NFD_REGISTER_STRATEGY(AsfStrategyTester);

BOOST_AUTO_TEST_SUITE(Fw)
BOOST_FIXTURE_TEST_SUITE(TestAsfStrategy, GlobalIoTimeFixture)

class AsfGridFixture : public GlobalIoTimeFixture
{
protected:
  explicit
  AsfGridFixture(const Name& params = AsfStrategy::getStrategyName(),
                 time::nanoseconds replyDelay = 0_ns)
    : parameters(params)
  {
    /*
     *                  +---------+
     *           +----->|  nodeB  |<------+
     *           |      +---------+       |
     *      10ms |                        | 10ms
     *           v                        v
     *      +---------+              +---------+
     *      |  nodeA  |              |  nodeC  |
     *      +---------+              +---------+
     *           ^                        ^
     *     100ms |                        | 100ms
     *           |      +---------+       |
     *           +----->|  nodeD  |<------+
     *                  +---------+
     */

    nodeA = topo.addForwarder("A");
    nodeB = topo.addForwarder("B");
    nodeC = topo.addForwarder("C");
    nodeD = topo.addForwarder("D");

    for (auto node : {nodeA, nodeB, nodeC, nodeD}) {
      topo.setStrategy<AsfStrategy>(node, Name("/"), parameters);
    }

    linkAB = topo.addLink("AB", 10_ms, {nodeA, nodeB});
    linkAD = topo.addLink("AD", 100_ms, {nodeA, nodeD});
    linkBC = topo.addLink("BC", 10_ms, {nodeB, nodeC});
    linkCD = topo.addLink("CD", 100_ms, {nodeC, nodeD});

    consumer = topo.addAppFace("c", nodeA);
    producer = topo.addAppFace("p", nodeC, PRODUCER_PREFIX);

    topo.addEchoProducer(producer->getClientFace(), PRODUCER_PREFIX, replyDelay);

    // Register producer prefix on consumer node
    topo.registerPrefix(nodeA, linkAB->getFace(nodeA), PRODUCER_PREFIX, 10);
    topo.registerPrefix(nodeA, linkAD->getFace(nodeA), PRODUCER_PREFIX, 5);
  }

  void
  runConsumer(size_t numInterests = 30)
  {
    topo.addIntervalConsumer(consumer->getClientFace(), PRODUCER_PREFIX, 1_s, numInterests);
    this->advanceClocks(10_ms, time::seconds(numInterests));
  }

protected:
  Name parameters;
  TopologyTester topo;

  TopologyNode nodeA;
  TopologyNode nodeB;
  TopologyNode nodeC;
  TopologyNode nodeD;

  shared_ptr<TopologyLink> linkAB;
  shared_ptr<TopologyLink> linkAD;
  shared_ptr<TopologyLink> linkBC;
  shared_ptr<TopologyLink> linkCD;

  shared_ptr<TopologyAppLink> consumer;
  shared_ptr<TopologyAppLink> producer;

  static inline const Name PRODUCER_PREFIX{"/hr/C"};
};

class AsfStrategyParametersGridFixture : public AsfGridFixture
{
protected:
  AsfStrategyParametersGridFixture()
    : AsfGridFixture(Name(AsfStrategy::getStrategyName())
                     .append("probing-interval~30000")
                     .append("max-timeouts~5"))
  {
  }
};

BOOST_FIXTURE_TEST_CASE(Basic, AsfGridFixture)
{
  // Both nodeB and nodeD have FIB entries to reach the producer
  topo.registerPrefix(nodeB, linkBC->getFace(nodeB), PRODUCER_PREFIX);
  topo.registerPrefix(nodeD, linkCD->getFace(nodeD), PRODUCER_PREFIX);

  runConsumer();

  // ASF should use the Face to nodeD because it has lower routing cost.
  // After 5 seconds, a probe Interest should be sent to the Face to nodeB,
  // and the probe should return Data quicker. ASF should then use the Face
  // to nodeB to forward the remaining Interests.
  BOOST_CHECK_EQUAL(consumer->getForwarderFace().getCounters().nOutData, 30);
  // Because of exploration, will forward to AB and AD simultaneously at least once
  BOOST_CHECK_GE(linkAB->getFace(nodeA).getCounters().nOutInterests, 25);
  BOOST_CHECK_LE(linkAD->getFace(nodeA).getCounters().nOutInterests, 6);

  // If the link from nodeA to nodeB fails, ASF should start using the Face
  // to nodeD again.
  linkAB->fail();

  runConsumer();
  // We experience 3 timeouts and marked AB as timed out
  BOOST_CHECK_EQUAL(consumer->getForwarderFace().getCounters().nOutData, 57);
  BOOST_CHECK_LE(linkAB->getFace(nodeA).getCounters().nOutInterests, 36);
  BOOST_CHECK_GE(linkAD->getFace(nodeA).getCounters().nOutInterests, 24);

  // If the link from nodeA to nodeB recovers, ASF should probe the Face
  // to nodeB and start using it again.
  linkAB->recover();

  // Advance time to ensure probing is due
  this->advanceClocks(10_ms, 10_s);

  runConsumer();
  BOOST_CHECK_EQUAL(consumer->getForwarderFace().getCounters().nOutData, 87);
  BOOST_CHECK_GE(linkAB->getFace(nodeA).getCounters().nOutInterests, 50);
  BOOST_CHECK_LE(linkAD->getFace(nodeA).getCounters().nOutInterests, 40);

  // If both links fail, nodeA should forward to the next hop with the lowest cost
  linkAB->fail();
  linkAD->fail();

  runConsumer();

  BOOST_CHECK_EQUAL(consumer->getForwarderFace().getCounters().nOutData, 87);
  BOOST_CHECK_LE(linkAB->getFace(nodeA).getCounters().nOutInterests, 65); // FIXME #3830
  BOOST_CHECK_GE(linkAD->getFace(nodeA).getCounters().nOutInterests, 57); // FIXME #3830
}

BOOST_FIXTURE_TEST_CASE(Nack, AsfGridFixture)
{
  // nodeB has a FIB entry to reach the producer, but nodeD does not
  topo.registerPrefix(nodeB, linkBC->getFace(nodeB), PRODUCER_PREFIX);

  // The strategy should first try to send to nodeD. But since nodeD does not have a route for
  // the producer's prefix, it should return a NO_ROUTE Nack. The strategy should then start using the Face to
  // nodeB.
  runConsumer();

  BOOST_CHECK_GE(linkAD->getFace(nodeA).getCounters().nInNacks, 1);
  BOOST_CHECK_EQUAL(consumer->getForwarderFace().getCounters().nOutData, 29);
  BOOST_CHECK_EQUAL(linkAB->getFace(nodeA).getCounters().nOutInterests, 29);

  // nodeD should receive 2 Interests: one for the very first Interest and
  // another from a probe
  BOOST_CHECK_GE(linkAD->getFace(nodeA).getCounters().nOutInterests, 2);
}

class AsfStrategyDelayedDataFixture : public AsfGridFixture
{
protected:
  AsfStrategyDelayedDataFixture()
    : AsfGridFixture(Name(AsfStrategy::getStrategyName()), 400_ms)
  {
  }
};

BOOST_FIXTURE_TEST_CASE(InterestForwarding, AsfStrategyDelayedDataFixture)
{
  Name name(PRODUCER_PREFIX);
  name.appendTimestamp();
  auto interest = makeInterest(name);

  topo.registerPrefix(nodeB, linkBC->getFace(nodeB), PRODUCER_PREFIX);
  topo.registerPrefix(nodeD, linkCD->getFace(nodeD), PRODUCER_PREFIX);

  // The first interest should go via link AD
  consumer->getClientFace().expressInterest(*interest, nullptr, nullptr, nullptr);
  this->advanceClocks(10_ms, 100_ms);
  BOOST_CHECK_EQUAL(linkAD->getFace(nodeA).getCounters().nOutInterests, 1);

  // Second interest should go via link AB
  interest->refreshNonce();
  consumer->getClientFace().expressInterest(*interest, nullptr, nullptr, nullptr);
  this->advanceClocks(10_ms, 100_ms);
  BOOST_CHECK_EQUAL(linkAB->getFace(nodeA).getCounters().nOutInterests, 1);

  // The third interest should again go via AD, since both the face from A is already used
  // and so asf should choose the earliest used face i.e. AD
  interest->refreshNonce();
  consumer->getClientFace().expressInterest(*interest, nullptr, nullptr, nullptr);
  this->advanceClocks(10_ms, 100_ms);
  BOOST_CHECK_EQUAL(linkAD->getFace(nodeA).getCounters().nOutInterests, 2);

  this->advanceClocks(time::milliseconds(500), time::seconds(5));
  BOOST_CHECK_EQUAL(linkAD->getFace(nodeA).getCounters().nInData, 1);
  BOOST_CHECK_EQUAL(linkAB->getFace(nodeA).getCounters().nInData, 1);
  BOOST_CHECK_EQUAL(consumer->getForwarderFace().getCounters().nOutData, 1);
}

BOOST_AUTO_TEST_CASE(Retransmission) // Bug #4874
{
  // Avoid clearing pit entry for those incoming interest that have pit entry but no next hops
  /*
   *        +---------+   10ms   +---------+
   *        |  nodeB  | ------>  |  nodeC  |
   *        +---------+          +---------+
   */

  const Name PRODUCER_PREFIX = "/pnr/C";
  TopologyTester topo;

  TopologyNode nodeB = topo.addForwarder("B"),
               nodeC = topo.addForwarder("C");

  for (auto node : {nodeB, nodeC}) {
    topo.setStrategy<AsfStrategy>(node);
  }

  auto linkBC = topo.addLink("BC", time::milliseconds(10), {nodeB, nodeC});

  auto consumer = topo.addAppFace("c", nodeB),
       producer = topo.addAppFace("p", nodeC, PRODUCER_PREFIX);

  topo.addEchoProducer(producer->getClientFace(), PRODUCER_PREFIX, 100_ms);

  Name name(PRODUCER_PREFIX);
  name.appendTimestamp();
  auto interest = makeInterest(name);

  auto& pit = topo.getForwarder(nodeB).getPit();
  auto pitEntry = pit.insert(*interest).first;

  topo.getForwarder(nodeB).onOutgoingInterest(*interest, linkBC->getFace(nodeB), pitEntry);
  this->advanceClocks(time::milliseconds(100));

  interest->refreshNonce();
  consumer->getClientFace().expressInterest(*interest, nullptr, nullptr, nullptr);
  this->advanceClocks(time::milliseconds(100));

  auto outRecord = pitEntry->findOutRecord(linkBC->getFace(nodeB));
  BOOST_CHECK(outRecord != pitEntry->out_end());

  this->advanceClocks(time::milliseconds(100));
  BOOST_CHECK_EQUAL(linkBC->getFace(nodeC).getCounters().nOutData, 1);
  BOOST_CHECK_EQUAL(linkBC->getFace(nodeB).getCounters().nInData, 1);
}

BOOST_AUTO_TEST_CASE(PerUpstreamSuppression)
{
  /*
   *                          +---------+
   *                     +----|  nodeB  |----+
   *                     |    +---------+    |
   *                50ms |                   | 50ms
   *                     |                   |
   *                +---------+   50ms  +---------+
   *                |  nodeA  | <-----> |  nodeP  |
   *                +---------+         +---------+
   */

  const Name PRODUCER_PREFIX = "/suppress/me";
  TopologyTester topo;

  TopologyNode nodeA = topo.addForwarder("A"),
               nodeB = topo.addForwarder("B"),
               nodeP = topo.addForwarder("P");

  for (auto node : {nodeA, nodeB, nodeP}) {
    topo.setStrategy<AsfStrategy>(node);
  }

  auto linkAB = topo.addLink("AB", 50_ms, {nodeA, nodeB});
  auto linkAP = topo.addLink("AP", 50_ms, {nodeA, nodeP});
  auto linkBP = topo.addLink("BP", 50_ms, {nodeB, nodeP});

  auto consumer = topo.addAppFace("cons", nodeA),
       producer = topo.addAppFace("prod", nodeP, PRODUCER_PREFIX);

  topo.addEchoProducer(producer->getClientFace(), PRODUCER_PREFIX);

  topo.registerPrefix(nodeA, linkAP->getFace(nodeA), PRODUCER_PREFIX, 10);
  topo.registerPrefix(nodeA, linkAB->getFace(nodeA), PRODUCER_PREFIX, 1);
  topo.registerPrefix(nodeB, linkBP->getFace(nodeB), PRODUCER_PREFIX, 1);

  auto& faceA2B = linkAB->getFace(nodeA);
  auto& faceA2P = linkAP->getFace(nodeA);

  Name name(PRODUCER_PREFIX);
  name.appendTimestamp();
  // very short lifetime to make it expire within the initial retx suppression period (10ms)
  auto interest = makeInterest(name, false, 5_ms);

  // 1st interest should be sent to B
  consumer->getClientFace().expressInterest(*interest, nullptr, nullptr, nullptr);
  this->advanceClocks(1_ms);
  BOOST_TEST(faceA2B.getCounters().nOutInterests == 1);
  BOOST_TEST(faceA2P.getCounters().nOutInterests == 0);

  // 2nd interest should be sent to P and NOT suppressed
  interest->setInterestLifetime(100_ms);
  interest->refreshNonce();
  consumer->getClientFace().expressInterest(*interest, nullptr, nullptr, nullptr);
  this->advanceClocks(1_ms);
  BOOST_TEST(faceA2B.getCounters().nOutInterests == 1);
  BOOST_TEST(faceA2P.getCounters().nOutInterests == 1);

  this->advanceClocks(1_ms);

  // 3rd interest should be suppressed
  // without suppression, it would have been sent again to B as that's the earliest out-record
  interest->refreshNonce();
  consumer->getClientFace().expressInterest(*interest, nullptr, nullptr, nullptr);
  this->advanceClocks(1_ms);
  BOOST_TEST(faceA2B.getCounters().nOutInterests == 1);
  BOOST_TEST(faceA2P.getCounters().nOutInterests == 1);

  this->advanceClocks(2_ms); // 1st interest should expire now

  // 4th interest should be suppressed
  // without suppression, it would have been sent again to B because the out-record expired
  interest->refreshNonce();
  consumer->getClientFace().expressInterest(*interest, nullptr, nullptr, nullptr);
  this->advanceClocks(1_ms);
  BOOST_TEST(faceA2B.getCounters().nOutInterests == 1);
  BOOST_TEST(faceA2P.getCounters().nOutInterests == 1);

  this->advanceClocks(5_ms); // suppression window ends

  // 5th interest is sent to B and is outside the suppression window
  interest->refreshNonce();
  consumer->getClientFace().expressInterest(*interest, nullptr, nullptr, nullptr);
  this->advanceClocks(1_ms);
  BOOST_TEST(faceA2B.getCounters().nOutInterests == 2);
  BOOST_TEST(faceA2P.getCounters().nOutInterests == 1);

  this->advanceClocks(10_ms);

  // 6th interest is sent to P and is outside the suppression window
  interest->refreshNonce();
  consumer->getClientFace().expressInterest(*interest, nullptr, nullptr, nullptr);
  this->advanceClocks(1_ms);
  BOOST_TEST(faceA2B.getCounters().nOutInterests == 2);
  BOOST_TEST(faceA2P.getCounters().nOutInterests == 2);
}

BOOST_AUTO_TEST_CASE(NoPitOutRecordAndProbeInterestNewNonce)
{
  /*                  +---------+
   *                  |  nodeD  |
   *                  +---------+
   *                       |
   *                       | 80ms
   *                       |
   *                       |
   *                  +---------+
   *           +----->|  nodeB  |<------+
   *           |      +---------+       |
   *      15ms |                        | 16ms
   *           v                        v
   *      +---------+              +---------+
   *      |  nodeA  |--------------|  nodeC  |
   *      +---------+     14ms      +---------+
   */

  const Name PRODUCER_PREFIX = "/ndn/edu/nodeD/ping";
  TopologyTester topo;

  TopologyNode nodeA = topo.addForwarder("A"),
               nodeB = topo.addForwarder("B"),
               nodeC = topo.addForwarder("C"),
               nodeD = topo.addForwarder("D");

  for (auto node : {nodeA, nodeB, nodeC, nodeD}) {
    topo.setStrategy<AsfStrategy>(node);
  }

  auto linkAB = topo.addLink("AB", 15_ms, {nodeA, nodeB}),
       linkAC = topo.addLink("AC", 14_ms, {nodeA, nodeC}),
       linkBC = topo.addLink("BC", 16_ms, {nodeB, nodeC}),
       linkBD = topo.addLink("BD", 80_ms, {nodeB, nodeD});

  auto ping = topo.addAppFace("c", nodeA);
  auto pingServer = topo.addAppFace("p", nodeD, PRODUCER_PREFIX);
  topo.addEchoProducer(pingServer->getClientFace());

  topo.registerPrefix(nodeA, linkAB->getFace(nodeA), PRODUCER_PREFIX, 15);
  topo.registerPrefix(nodeA, linkAC->getFace(nodeA), PRODUCER_PREFIX, 14);
  topo.registerPrefix(nodeC, linkBC->getFace(nodeC), PRODUCER_PREFIX, 16);
  topo.registerPrefix(nodeB, linkBD->getFace(nodeB), PRODUCER_PREFIX, 80);

  // Send 6 interest since probes can be scheduled b/w 0-5 seconds
  for (int i = 1; i < 7; i++) {
    // Send ping number i
    Name name(PRODUCER_PREFIX);
    name.appendTimestamp();
    auto interest = makeInterest(name);
    ping->getClientFace().expressInterest(*interest, nullptr, nullptr, nullptr);
    auto nonce = interest->getNonce();

    // Don't know when the probe will be triggered since it is random between 0-5 seconds
    // or whether it will be triggered for this interest
    for (int j = 1; j <= 1000 && linkAB->getFace(nodeA).getCounters().nOutInterests != 1; ++j) {
      this->advanceClocks(1_ms);
    }

    // Check if probe is sent to B else send another ping
    if (linkAB->getFace(nodeA).getCounters().nOutInterests == 1) {
      // Get pitEntry of node A
      auto pitEntry = topo.getForwarder(nodeA).getPit().find(*interest);
      // Get outRecord associated with face towards B
      auto outRecord = pitEntry->findOutRecord(linkAB->getFace(nodeA));
      BOOST_REQUIRE(outRecord != pitEntry->out_end());

      // Check that Nonce of interest is not equal to Nonce of Probe
      BOOST_CHECK_NE(nonce, outRecord->getLastNonce());

      // B should not have received the probe interest yet
      BOOST_CHECK_EQUAL(linkAB->getFace(nodeB).getCounters().nInInterests, 0);

      // i-1 interests through B when no probe
      BOOST_CHECK_EQUAL(linkBD->getFace(nodeB).getCounters().nOutInterests, i - 1);

      // After 15ms, B should get the probe interest
      this->advanceClocks(1_ms, 15_ms);
      BOOST_CHECK_EQUAL(linkAB->getFace(nodeB).getCounters().nInInterests, 1);
      BOOST_CHECK_EQUAL(linkBD->getFace(nodeB).getCounters().nOutInterests, i);

      pitEntry = topo.getForwarder(nodeB).getPit().find(*interest);

      // Get outRecord associated with face towards D.
      outRecord = pitEntry->findOutRecord(linkBD->getFace(nodeB));
      BOOST_CHECK(outRecord != pitEntry->out_end());

      // RTT between B and D
      this->advanceClocks(5_ms, 160_ms);
      outRecord = pitEntry->findOutRecord(linkBD->getFace(nodeB));

      BOOST_CHECK_EQUAL(linkBD->getFace(nodeB).getCounters().nInData, i);

      BOOST_CHECK(outRecord == pitEntry->out_end());

      // Data is returned for the ping after 15 ms - will result in false measurement
      // 14+16-15 = 15ms
      // Since outRecord == pitEntry->out_end()
      this->advanceClocks(1_ms, 15_ms);
      BOOST_CHECK_EQUAL(linkBD->getFace(nodeB).getCounters().nInData, i+1);

      break;
    }
  }
}

BOOST_FIXTURE_TEST_CASE(IgnoreTimeouts, AsfStrategyParametersGridFixture)
{
  // Both nodeB and nodeD have FIB entries to reach the producer
  topo.registerPrefix(nodeB, linkBC->getFace(nodeB), PRODUCER_PREFIX);
  topo.registerPrefix(nodeD, linkCD->getFace(nodeD), PRODUCER_PREFIX);

  // Send 15 interests let it change to use the 10 ms link
  runConsumer(15);

  uint64_t outInterestsBeforeFailure = linkAD->getFace(nodeA).getCounters().nOutInterests;

  // Bring down 10 ms link
  linkAB->fail();

  // Send 5 interests, after the last one it will record the timeout
  // ready to switch for the next interest
  runConsumer(5);

  // Check that link has not been switched to 100 ms because max-timeouts = 5
  BOOST_CHECK_EQUAL(linkAD->getFace(nodeA).getCounters().nOutInterests, outInterestsBeforeFailure);

  // Send 5 interests, check that 100 ms link is used
  runConsumer(5);

  BOOST_CHECK_EQUAL(linkAD->getFace(nodeA).getCounters().nOutInterests, outInterestsBeforeFailure + 5);
}

BOOST_FIXTURE_TEST_CASE(ProbingInterval, AsfStrategyParametersGridFixture)
{
  // Both nodeB and nodeD have FIB entries to reach the producer
  topo.registerPrefix(nodeB, linkBC->getFace(nodeB), PRODUCER_PREFIX);
  topo.registerPrefix(nodeD, linkCD->getFace(nodeD), PRODUCER_PREFIX);

  // Send 6 interests let it change to use the 10 ms link
  runConsumer(6);

  auto linkAC = topo.addLink("AC", 5_ms, {nodeA, nodeD});
  topo.registerPrefix(nodeA, linkAC->getFace(nodeA), PRODUCER_PREFIX, 1);

  BOOST_CHECK_EQUAL(linkAC->getFace(nodeA).getCounters().nOutInterests, 0);

  // After 30 seconds a probe would be sent that would switch make ASF switch
  runConsumer(30);

  BOOST_CHECK_EQUAL(linkAC->getFace(nodeA).getCounters().nOutInterests, 1);
}

BOOST_AUTO_TEST_CASE(Parameters)
{
  FaceTable faceTable;
  Forwarder forwarder{faceTable};

  auto checkValidity = [&] (std::string_view parameters, bool isCorrect) {
    BOOST_TEST_INFO_SCOPE(parameters);
    Name strategyName(Name(AsfStrategy::getStrategyName()).append(Name(parameters)));
    std::unique_ptr<AsfStrategy> strategy;
    if (isCorrect) {
      strategy = make_unique<AsfStrategy>(forwarder, strategyName);
      BOOST_CHECK(strategy->m_retxSuppression != nullptr);
    }
    else {
      BOOST_CHECK_THROW(make_unique<AsfStrategy>(forwarder, strategyName), std::invalid_argument);
    }
    return strategy;
  };

  auto strategy = checkValidity("", true);
  BOOST_TEST(strategy->m_probing.getProbingInterval() == 60_s);
  BOOST_TEST(strategy->m_nMaxTimeouts == 3);
  BOOST_TEST(strategy->m_measurements.getMeasurementsLifetime() == 5_min);
  strategy = checkValidity("/probing-interval~30000/max-timeouts~5/measurements-lifetime~120000", true);
  BOOST_TEST(strategy->m_probing.getProbingInterval() == 30_s);
  BOOST_TEST(strategy->m_nMaxTimeouts == 5);
  BOOST_TEST(strategy->m_measurements.getMeasurementsLifetime() == 2_min);
  strategy = checkValidity("/max-timeouts~5/probing-interval~30000", true);
  BOOST_TEST(strategy->m_probing.getProbingInterval() == 30_s);
  BOOST_TEST(strategy->m_nMaxTimeouts == 5);
  BOOST_TEST(strategy->m_measurements.getMeasurementsLifetime() == 5_min);
  strategy = checkValidity("/max-timeouts~5/measurements-lifetime~120000", true);
  BOOST_TEST(strategy->m_nMaxTimeouts == 5);
  BOOST_TEST(strategy->m_measurements.getMeasurementsLifetime() == 2_min);
  strategy = checkValidity("/probing-interval~30000/measurements-lifetime~120000", true);
  BOOST_TEST(strategy->m_probing.getProbingInterval() == 30_s);
  BOOST_TEST(strategy->m_measurements.getMeasurementsLifetime() == 2_min);
  strategy = checkValidity("/probing-interval~1000", true);
  BOOST_TEST(strategy->m_probing.getProbingInterval() == 1_s);
  BOOST_TEST(strategy->m_nMaxTimeouts == 3);
  BOOST_TEST(strategy->m_measurements.getMeasurementsLifetime() == 5_min);
  strategy = checkValidity("/max-timeouts~0", true);
  BOOST_TEST(strategy->m_probing.getProbingInterval() == 60_s);
  BOOST_TEST(strategy->m_nMaxTimeouts == 0);
  BOOST_TEST(strategy->m_measurements.getMeasurementsLifetime() == 5_min);
  strategy = checkValidity("/measurements-lifetime~120000", true);
  BOOST_TEST(strategy->m_probing.getProbingInterval() == 60_s);
  BOOST_TEST(strategy->m_nMaxTimeouts == 3);
  BOOST_TEST(strategy->m_measurements.getMeasurementsLifetime() == 2_min);
  BOOST_TEST(strategy->m_retxSuppression->m_initialInterval == fw::RetxSuppressionExponential::DEFAULT_INITIAL_INTERVAL);
  BOOST_TEST(strategy->m_retxSuppression->m_maxInterval == fw::RetxSuppressionExponential::DEFAULT_MAX_INTERVAL);
  BOOST_TEST(strategy->m_retxSuppression->m_multiplier == fw::RetxSuppressionExponential::DEFAULT_MULTIPLIER);

  checkValidity("/probing-interval~500", false); // minimum is 1 second
  checkValidity("/probing-interval~-5000", false);
  checkValidity("/max-timeouts~-1", false);
  checkValidity("/max-timeouts~ -1", false);
  checkValidity("/max-timeouts~1-0", false);
  checkValidity("/max-timeouts~1/probing-interval~-30000", false);
  checkValidity("/probing-interval~foo", false);
  checkValidity("/max-timeouts~1~2", false);
  checkValidity("/measurements-lifetime~1000", false); //Minimum is 60s by default
  //Measurement lifetime must be greater than probing interval
  checkValidity("/measurements-lifetime~1000/probing-interval~30000", false);
  checkValidity("/measurements-lifetime~-120000", false);
  checkValidity("/measurements-lifetime~ -120000", false);
  checkValidity("/measurements-lifetime~0-120000", false);
  checkValidity("/max-timeouts~1/measurements-lifetime~-120000", false);
  checkValidity("/probing-interval~30000/measurements-lifetime~-120000", false);
  checkValidity("/max-timeouts~1/probing-interval~30000/measurements-lifetime~-120000", false);
}

BOOST_AUTO_TEST_CASE(FaceRankingForForwarding)
{
  const Name PRODUCER_PREFIX = "/ndn/edu/nodeD/ping";
  AsfStrategy::FaceStatsForwardingSet rankedFaces;

  //Group 1- Working Measured Faces
  FaceInfo group1_a(nullptr);
  group1_a.recordRtt(25_ms);
  DummyFace face1_a;
  face1_a.setId(1);
  rankedFaces.insert({&face1_a, group1_a.getLastRtt(), group1_a.getSrtt(), 0});
  // Higher FaceId
  FaceInfo group1_b(nullptr);
  group1_b.recordRtt(25_ms);
  DummyFace face1_b;
  face1_b.setId(2);
  rankedFaces.insert({&face1_b, group1_b.getLastRtt(), group1_b.getSrtt(), 0});
  //Higher SRTT
  FaceInfo group1_c(nullptr);
  group1_c.recordRtt(30_ms);
  DummyFace face1_c;
  face1_c.setId(3);
  rankedFaces.insert({&face1_c, group1_c.getLastRtt(), group1_c.getSrtt(), 0});
  //Higher SRTT/Cost
  FaceInfo group1_d(nullptr);
  group1_d.recordRtt(30_ms);
  DummyFace face1_d;
  face1_d.setId(4);
  rankedFaces.insert({&face1_d, group1_d.getLastRtt(), group1_d.getSrtt(), 1});
  //Group 2- Unmeasured Faces
  FaceInfo group2_a(nullptr);
  DummyFace face2_a;
  face2_a.setId(5);
  rankedFaces.insert({&face2_a, FaceInfo::RTT_NO_MEASUREMENT,
                      FaceInfo::RTT_NO_MEASUREMENT, 0});
  //Higher FaceId
  FaceInfo group2_b(nullptr);
  DummyFace face2_b;
  face2_b.setId(6);
  rankedFaces.insert({&face2_b, FaceInfo::RTT_NO_MEASUREMENT,
                      FaceInfo::RTT_NO_MEASUREMENT, 0});
  //Higher Cost
  FaceInfo group2_c(nullptr);
  DummyFace face2_c;
  face2_c.setId(7);
  rankedFaces.insert({&face2_c, FaceInfo::RTT_NO_MEASUREMENT,
                      FaceInfo::RTT_NO_MEASUREMENT, 1});
  //Group 3- Timeout Faces
  //Lowest cost, high SRTT
  FaceInfo group3_a(nullptr);
  group3_a.recordRtt(30_ms);
  group3_a.recordTimeout(PRODUCER_PREFIX);
  DummyFace face3_a;
  face3_a.setId(8);
  rankedFaces.insert({&face3_a, group3_a.getLastRtt(), group3_a.getSrtt(), 0});
  //Lowest cost, lower SRTT, higher FaceId
  FaceInfo group3_b(nullptr);
  group3_b.recordRtt(30_ms);
  group3_b.recordTimeout(PRODUCER_PREFIX);
  DummyFace face3_b;
  face3_b.setId(9);
  rankedFaces.insert({&face3_b, group3_b.getLastRtt(), group3_b.getSrtt(), 0});
  //Lowest cost, higher SRTT, higher FaceId
  FaceInfo group3_c(nullptr);
  group3_c.recordRtt(45_ms);
  group3_c.recordTimeout(PRODUCER_PREFIX);
  DummyFace face3_c;
  face3_c.setId(10);
  rankedFaces.insert({&face3_c, group3_c.getLastRtt(), group3_c.getSrtt(), 0});
  //Lowest cost, no SRTT, higher FaceId
  FaceInfo group3_d(nullptr);
  group3_d.recordTimeout(PRODUCER_PREFIX);
  DummyFace face3_d;
  face3_d.setId(11);
  rankedFaces.insert({&face3_d, group3_d.getLastRtt(), FaceInfo::RTT_NO_MEASUREMENT, 0});
  //Higher cost, lower SRTT, higher FaceId
  FaceInfo group3_e(nullptr);
  group3_e.recordRtt(15_ms);
  group3_e.recordTimeout(PRODUCER_PREFIX);
  DummyFace face3_e;
  face3_e.setId(12);
  rankedFaces.insert({&face3_e, group3_e.getLastRtt(), group3_e.getSrtt(), 1});
  //Higher cost, higher SRTT, higher FaceId
  FaceInfo group3_f(nullptr);
  group3_f.recordRtt(45_ms);
  group3_f.recordTimeout(PRODUCER_PREFIX);
  DummyFace face3_f;
  face3_f.setId(13);
  rankedFaces.insert({&face3_f, group3_f.getLastRtt(), group3_f.getSrtt(), 1});
  //Higher cost, no SRTT, higher FaceId
  FaceInfo group3_g(nullptr);
  group3_g.recordTimeout(PRODUCER_PREFIX);
  DummyFace face3_g;
  face3_g.setId(14);
  rankedFaces.insert({&face3_g, FaceInfo::RTT_TIMEOUT,
                      FaceInfo::RTT_NO_MEASUREMENT, 1});
  auto face = rankedFaces.begin();
  //Group 1 - Working Measured Faces
  BOOST_CHECK_EQUAL(face->rtt, group1_a.getLastRtt());
  BOOST_CHECK_EQUAL(face->srtt, group1_a.getSrtt());
  BOOST_CHECK_EQUAL(face->face->getId(), 1);
  face++;
  BOOST_CHECK_EQUAL(face->rtt, group1_b.getLastRtt());
  BOOST_CHECK_EQUAL(face->srtt, group1_b.getSrtt());
  BOOST_CHECK_EQUAL(face->face->getId(), 2);
  face++;
  BOOST_CHECK_EQUAL(face->rtt, group1_c.getLastRtt());
  BOOST_CHECK_EQUAL(face->srtt, group1_c.getSrtt());
  BOOST_CHECK_EQUAL(face->face->getId(), 3);
  face++;
  BOOST_CHECK_EQUAL(face->rtt, group1_d.getLastRtt());
  BOOST_CHECK_EQUAL(face->srtt, group1_d.getSrtt());
  BOOST_CHECK_EQUAL(face->face->getId(), 4);
  face++;
  //Group 2 - Unmeasured Faces
  BOOST_CHECK_EQUAL(face->rtt, FaceInfo::RTT_NO_MEASUREMENT);
  BOOST_CHECK_EQUAL(face->srtt, FaceInfo::RTT_NO_MEASUREMENT);
  BOOST_CHECK_EQUAL(face->face->getId(), 5);
  face++;
  BOOST_CHECK_EQUAL(face->rtt, FaceInfo::RTT_NO_MEASUREMENT);
  BOOST_CHECK_EQUAL(face->srtt, FaceInfo::RTT_NO_MEASUREMENT);
  BOOST_CHECK_EQUAL(face->face->getId(), 6);
  face++;
  BOOST_CHECK_EQUAL(face->rtt, FaceInfo::RTT_NO_MEASUREMENT);
  BOOST_CHECK_EQUAL(face->srtt, FaceInfo::RTT_NO_MEASUREMENT);
  BOOST_CHECK_EQUAL(face->face->getId(), 7);
  face++;
  //Group 3 - Timeout
  BOOST_CHECK_EQUAL(face->rtt, FaceInfo::RTT_TIMEOUT);
  BOOST_CHECK_EQUAL(face->srtt, group3_a.getSrtt());
  BOOST_CHECK_EQUAL(face->face->getId(), 8);
  face++;
  BOOST_CHECK_EQUAL(face->rtt, FaceInfo::RTT_TIMEOUT);
  BOOST_CHECK_EQUAL(face->srtt, group3_b.getSrtt());
  BOOST_CHECK_EQUAL(face->face->getId(), 9);
  face++;
  BOOST_CHECK_EQUAL(face->rtt, FaceInfo::RTT_TIMEOUT);
  BOOST_CHECK_EQUAL(face->srtt, group3_c.getSrtt());
  BOOST_CHECK_EQUAL(face->face->getId(), 10);
  face++;
  BOOST_CHECK_EQUAL(face->rtt, FaceInfo::RTT_TIMEOUT);
  BOOST_CHECK_EQUAL(face->srtt, FaceInfo::RTT_NO_MEASUREMENT);
  BOOST_CHECK_EQUAL(face->face->getId(), 11);
  face++;
  BOOST_CHECK_EQUAL(face->rtt, FaceInfo::RTT_TIMEOUT);
  BOOST_CHECK_EQUAL(face->srtt, group3_e.getSrtt());
  BOOST_CHECK_EQUAL(face->face->getId(), 12);
  face++;
  BOOST_CHECK_EQUAL(face->rtt, FaceInfo::RTT_TIMEOUT);
  BOOST_CHECK_EQUAL(face->srtt, group3_f.getSrtt());
  BOOST_CHECK_EQUAL(face->face->getId(), 13);
  face++;
  BOOST_CHECK_EQUAL(face->rtt, FaceInfo::RTT_TIMEOUT);
  BOOST_CHECK_EQUAL(face->srtt, FaceInfo::RTT_NO_MEASUREMENT);
  BOOST_CHECK_EQUAL(face->face->getId(), 14);
  // face++;
}

BOOST_AUTO_TEST_CASE(FaceRankingForProbing)
{
  const Name PRODUCER_PREFIX = "/ndn/edu/nodeD/ping";
  fw::asf::ProbingModule::FaceStatsProbingSet rankedFaces;

  //Group 2- Unmeasured Faces
  FaceInfo group2_a(nullptr);
  DummyFace face2_a;
  face2_a.setId(1);
  rankedFaces.insert({&face2_a, FaceInfo::RTT_NO_MEASUREMENT,
                      FaceInfo::RTT_NO_MEASUREMENT, 0});
  //Higher FaceId
  FaceInfo group2_b(nullptr);
  DummyFace face2_b;
  face2_b.setId(2);
  rankedFaces.insert({&face2_b, FaceInfo::RTT_NO_MEASUREMENT,
                      FaceInfo::RTT_NO_MEASUREMENT, 0});
  //Higher Cost
  FaceInfo group2_c(nullptr);
  DummyFace face2_c;
  face2_c.setId(3);
  rankedFaces.insert({&face2_c, FaceInfo::RTT_NO_MEASUREMENT,
                      FaceInfo::RTT_NO_MEASUREMENT, 1});

  //Group 1- Working Measured Faces
  FaceInfo group1_a(nullptr);
  group1_a.recordRtt(25_ms);
  DummyFace face1_a;
  face1_a.setId(4);
  rankedFaces.insert({&face1_a, group1_a.getLastRtt(), group1_a.getSrtt(), 0});
  // Higher FaceId
  FaceInfo group1_b(nullptr);
  group1_b.recordRtt(25_ms);
  DummyFace face1_b;
  face1_b.setId(5);
  rankedFaces.insert({&face1_b, group1_b.getLastRtt(), group1_b.getSrtt(), 0});
  //Higher SRTT
  FaceInfo group1_c(nullptr);
  group1_c.recordRtt(30_ms);
  DummyFace face1_c;
  face1_c.setId(6);
  rankedFaces.insert({&face1_c, group1_c.getLastRtt(), group1_c.getSrtt(), 0});
  //Higher SRTT/Cost
  FaceInfo group1_d(nullptr);
  group1_d.recordRtt(30_ms);
  DummyFace face1_d;
  face1_d.setId(7);
  rankedFaces.insert({&face1_d, group1_d.getLastRtt(), group1_d.getSrtt(), 1});

  //Group 3- Timeout Faces
  //Lowest cost, high SRTT
  FaceInfo group3_a(nullptr);
  group3_a.recordRtt(30_ms);
  group3_a.recordTimeout(PRODUCER_PREFIX);
  DummyFace face3_a;
  face3_a.setId(8);
  rankedFaces.insert({&face3_a, group3_a.getLastRtt(), group3_a.getSrtt(), 0});
  //Lowest cost, lower SRTT, higher FaceId
  FaceInfo group3_b(nullptr);
  group3_b.recordRtt(30_ms);
  group3_b.recordTimeout(PRODUCER_PREFIX);
  DummyFace face3_b;
  face3_b.setId(9);
  rankedFaces.insert({&face3_b, group3_b.getLastRtt(), group3_b.getSrtt(), 0});
  //Lowest cost, higher SRTT, higher FaceId
  FaceInfo group3_c(nullptr);
  group3_c.recordRtt(45_ms);
  group3_c.recordTimeout(PRODUCER_PREFIX);
  DummyFace face3_c;
  face3_c.setId(10);
  rankedFaces.insert({&face3_c, group3_c.getLastRtt(), group3_c.getSrtt(), 0});
  //Lowest cost, no SRTT, higher FaceId
  FaceInfo group3_d(nullptr);
  group3_d.recordTimeout(PRODUCER_PREFIX);
  DummyFace face3_d;
  face3_d.setId(11);
  rankedFaces.insert({&face3_d, group3_d.getLastRtt(), FaceInfo::RTT_NO_MEASUREMENT, 0});
  //Higher cost, lower SRTT, higher FaceId
  FaceInfo group3_e(nullptr);
  group3_e.recordRtt(15_ms);
  group3_e.recordTimeout(PRODUCER_PREFIX);
  DummyFace face3_e;
  face3_e.setId(12);
  rankedFaces.insert({&face3_e, group3_e.getLastRtt(), group3_e.getSrtt(), 1});
  //Higher cost, higher SRTT, higher FaceId
  FaceInfo group3_f(nullptr);
  group3_f.recordRtt(45_ms);
  group3_f.recordTimeout(PRODUCER_PREFIX);
  DummyFace face3_f;
  face3_f.setId(13);
  rankedFaces.insert({&face3_f, group3_f.getLastRtt(), group3_f.getSrtt(), 1});
  //Higher cost, no SRTT, higher FaceId
  FaceInfo group3_g(nullptr);
  group3_g.recordTimeout(PRODUCER_PREFIX);
  DummyFace face3_g;
  face3_g.setId(14);
  rankedFaces.insert({&face3_g, group3_g.getLastRtt(),
                      FaceInfo::RTT_NO_MEASUREMENT, 1});
  auto face = rankedFaces.begin();

  //Group 2 - Unmeasured Faces
  BOOST_CHECK_EQUAL(face->rtt, FaceInfo::RTT_NO_MEASUREMENT);
  BOOST_CHECK_EQUAL(face->srtt, FaceInfo::RTT_NO_MEASUREMENT);
  BOOST_CHECK_EQUAL(face->face->getId(), 1);
  face++;
  BOOST_CHECK_EQUAL(face->rtt, FaceInfo::RTT_NO_MEASUREMENT);
  BOOST_CHECK_EQUAL(face->srtt, FaceInfo::RTT_NO_MEASUREMENT);
  BOOST_CHECK_EQUAL(face->face->getId(), 2);
  face++;
  BOOST_CHECK_EQUAL(face->rtt, FaceInfo::RTT_NO_MEASUREMENT);
  BOOST_CHECK_EQUAL(face->srtt, FaceInfo::RTT_NO_MEASUREMENT);
  BOOST_CHECK_EQUAL(face->face->getId(), 3);
  face++;
  //Group 1 - Working Measured Faces
  BOOST_CHECK_EQUAL(face->rtt, group1_a.getLastRtt());
  BOOST_CHECK_EQUAL(face->srtt, group1_a.getSrtt());
  BOOST_CHECK_EQUAL(face->face->getId(), 4);
  face++;
  BOOST_CHECK_EQUAL(face->rtt, group1_b.getLastRtt());
  BOOST_CHECK_EQUAL(face->srtt, group1_b.getSrtt());
  BOOST_CHECK_EQUAL(face->face->getId(), 5);
  face++;
  BOOST_CHECK_EQUAL(face->rtt, group1_c.getLastRtt());
  BOOST_CHECK_EQUAL(face->srtt, group1_c.getSrtt());
  BOOST_CHECK_EQUAL(face->face->getId(), 6);
  face++;
  BOOST_CHECK_EQUAL(face->rtt, group1_d.getLastRtt());
  BOOST_CHECK_EQUAL(face->srtt, group1_d.getSrtt());
  BOOST_CHECK_EQUAL(face->face->getId(), 7);
  face++;
  //Group 3 - Timeout
  BOOST_CHECK_EQUAL(face->rtt, FaceInfo::RTT_TIMEOUT);
  BOOST_CHECK_EQUAL(face->srtt, group3_a.getSrtt());
  BOOST_CHECK_EQUAL(face->face->getId(), 8);
  face++;
  BOOST_CHECK_EQUAL(face->rtt, FaceInfo::RTT_TIMEOUT);
  BOOST_CHECK_EQUAL(face->srtt, group3_b.getSrtt());
  BOOST_CHECK_EQUAL(face->face->getId(), 9);
  face++;
  BOOST_CHECK_EQUAL(face->rtt, FaceInfo::RTT_TIMEOUT);
  BOOST_CHECK_EQUAL(face->srtt, group3_c.getSrtt());
  BOOST_CHECK_EQUAL(face->face->getId(), 10);
  face++;
  BOOST_CHECK_EQUAL(face->rtt, FaceInfo::RTT_TIMEOUT);
  BOOST_CHECK_EQUAL(face->srtt, FaceInfo::RTT_NO_MEASUREMENT);
  BOOST_CHECK_EQUAL(face->face->getId(), 11);
  face++;
  BOOST_CHECK_EQUAL(face->rtt, FaceInfo::RTT_TIMEOUT);
  BOOST_CHECK_EQUAL(face->srtt, group3_e.getSrtt());
  BOOST_CHECK_EQUAL(face->face->getId(), 12);
  face++;
  BOOST_CHECK_EQUAL(face->rtt, FaceInfo::RTT_TIMEOUT);
  BOOST_CHECK_EQUAL(face->srtt, group3_f.getSrtt());
  BOOST_CHECK_EQUAL(face->face->getId(), 13);
  face++;
  BOOST_CHECK_EQUAL(face->rtt, FaceInfo::RTT_TIMEOUT);
  BOOST_CHECK_EQUAL(face->srtt, FaceInfo::RTT_NO_MEASUREMENT);
  BOOST_CHECK_EQUAL(face->face->getId(), 14);
  // face++;
}

BOOST_AUTO_TEST_SUITE_END() // TestAsfStrategy
BOOST_AUTO_TEST_SUITE_END() // Fw

} // namespace nfd::tests
