#include "labs/set_assoc_cache.hh"

#include "base/compiler.hh"
#include "base/random.hh"
#include "debug/SetAssocCache.hh"
#include "sim/system.hh"

namespace gem5 {

SetAssocCache::SetAssocCache(const SetAssocCacheParams &params)
    : ClockedObject(params), latency(params.latency),
      blockSize(params.system->cacheLineSize()),
      capacity(params.size / blockSize),
      memPort(params.name + ".mem_side", this), blocked(false),
      originalPacket(nullptr), waitingPortId(-1), stats(this) {
  // Since the CPU side ports are a vector of ports, create an instance of
  // the CPUSidePort for each connection. This member of params is
  // automatically created depending on the name of the vector port and
  // holds the number of connections to this port name
  for (int i = 0; i < params.port_cpu_side_connection_count; ++i) {
    cpuPorts.emplace_back(name() + csprintf(".cpu_side[%d]", i), i, this);
  }
}

Port &SetAssocCache::getPort(const std::string &if_name, PortID idx) {
  if (if_name == "mem_side") {
    panic_if(idx != InvalidPortID,
      "Mem side of fully cache not a vector port");
    return memPort;
  } else if (if_name == "cpu_side" && idx < cpuPorts.size()) {
    // We should have already created all of the ports in the constructor
    return cpuPorts[idx];
  } else {
    // pass it along to our super class
    return ClockedObject::getPort(if_name, idx);
  }
}

void SetAssocCache::CPUSidePort::sendPacket(PacketPtr pkt) {
  // Note: This flow control is very fully since the cache is blocking.

  panic_if(blockedPacket != nullptr, "Should never try to send if blocked!");

  // If we can't send the packet across the port, store it for later.
  DPRINTF(SetAssocCache, "Sending %s to CPU\n", pkt->print());
  if (!sendTimingResp(pkt)) {
    DPRINTF(SetAssocCache, "failed!\n");
    blockedPacket = pkt;
  }
}

AddrRangeList SetAssocCache::CPUSidePort::getAddrRanges() const {
  return owner->getAddrRanges();
}

void SetAssocCache::CPUSidePort::trySendRetry() {
  if (needRetry && blockedPacket == nullptr) {
    // Only send a retry if the port is now completely free
    needRetry = false;
    DPRINTF(SetAssocCache, "Sending retry req.\n");
    sendRetryReq();
  }
}

void SetAssocCache::CPUSidePort::recvFunctional(PacketPtr pkt) {
  // Just forward to the cache.
  return owner->handleFunctional(pkt);
}

bool SetAssocCache::CPUSidePort::recvTimingReq(PacketPtr pkt) {
  DPRINTF(SetAssocCache, "Got request %s\n", pkt->print());

  if (blockedPacket || needRetry) {
    // The cache may not be able to send a reply if this is blocked
    DPRINTF(SetAssocCache, "Request blocked\n");
    needRetry = true;
    return false;
  }
  // Just forward to the cache.
  if (!owner->handleRequest(pkt, id)) {
    DPRINTF(SetAssocCache, "Request failed\n");
    // stalling
    needRetry = true;
    return false;
  } else {
    DPRINTF(SetAssocCache, "Request succeeded\n");
    return true;
  }
}

void SetAssocCache::CPUSidePort::recvRespRetry() {
  // We should have a blocked packet if this function is called.
  assert(blockedPacket != nullptr);

  // Grab the blocked packet.
  PacketPtr pkt = blockedPacket;
  blockedPacket = nullptr;

  DPRINTF(SetAssocCache, "Retrying response pkt %s\n", pkt->print());
  // Try to resend it. It's possible that it fails again.
  sendPacket(pkt);

  // We may now be able to accept new packets
  trySendRetry();
}

void SetAssocCache::MemSidePort::sendPacket(PacketPtr pkt) {
  // Note: This flow control is very fully since the cache is blocking.

  panic_if(blockedPacket != nullptr, "Should never try to send if blocked!");

  // If we can't send the packet across the port, store it for later.
  if (!sendTimingReq(pkt)) {
    blockedPacket = pkt;
  }
}

bool SetAssocCache::MemSidePort::recvTimingResp(PacketPtr pkt) {
  // Just forward to the cache.
  return owner->handleResponse(pkt);
}

void SetAssocCache::MemSidePort::recvReqRetry() {
  // We should have a blocked packet if this function is called.
  assert(blockedPacket != nullptr);

  // Grab the blocked packet.
  PacketPtr pkt = blockedPacket;
  blockedPacket = nullptr;

  // Try to resend it. It's possible that it fails again.
  sendPacket(pkt);
}

void SetAssocCache::MemSidePort::recvRangeChange() {
  owner->sendRangeChange();
}

bool SetAssocCache::handleRequest(PacketPtr pkt, int port_id) {
  if (blocked) {
    // There is currently an outstanding request so we can't respond. Stall
    return false;
  }

  DPRINTF(SetAssocCache, "Got request for addr %#x\n", pkt->getAddr());

  // This cache is now blocked waiting for the response to this packet.
  blocked = true;

  // Store the port for when we get the response
  assert(waitingPortId == -1);
  waitingPortId = port_id;

  // Schedule an event after cache access latency to actually access
  schedule(new EventFunctionWrapper([this, pkt] { accessTiming(pkt); },
                                    name() + ".accessEvent", true),
           clockEdge(latency));

  return true;
}

bool SetAssocCache::handleResponse(PacketPtr pkt) {
  assert(blocked);
  DPRINTF(SetAssocCache, "Got response for addr %#x\n", pkt->getAddr());

  // For now assume that inserts are off of the critical path and don't count
  // for any added latency.
  insert(pkt);

  stats.missLatency.sample(curTick() - missTime);

  // If we had to upgrade the request packet to a full cache line, now we
  // can use that packet to construct the response.
  if (originalPacket != nullptr) {
    DPRINTF(SetAssocCache, "Copying data from new packet to old\n");
    // We had to upgrade a previous packet. We can functionally deal with
    // the cache access now. It better be a hit.
    [[maybe_unused]] bool hit = accessFunctional(originalPacket);
    panic_if(!hit, "Should always hit after inserting");
    originalPacket->makeResponse();
    delete pkt; // We may need to delay this, I'm not sure.
    pkt = originalPacket;
    originalPacket = nullptr;
  } // else, pkt contains the data it needs

  sendResponse(pkt);

  return true;
}

void SetAssocCache::sendResponse(PacketPtr pkt) {
  assert(blocked);
  DPRINTF(SetAssocCache, "Sending resp for addr %#x\n", pkt->getAddr());

  int port = waitingPortId;

  // The packet is now done. We're about to put it in the port, no need for
  // this object to continue to stall.
  // We need to free the resource before sending the packet in case the CPU
  // tries to send another request immediately (e.g., in the same callchain).
  blocked = false;
  waitingPortId = -1;

  // Simply forward to the memory port
  cpuPorts[port].sendPacket(pkt);

  // For each of the cpu ports, if it needs to send a retry, it should do it
  // now since this memory object may be unblocked now.
  for (auto &port : cpuPorts) {
    port.trySendRetry();
  }
}

void SetAssocCache::handleFunctional(PacketPtr pkt) {
  if (accessFunctional(pkt)) {
    pkt->makeResponse();
  } else {
    memPort.sendFunctional(pkt);
  }
}

void SetAssocCache::accessTiming(PacketPtr pkt) {
  bool hit = accessFunctional(pkt);

  DPRINTF(SetAssocCache, "%s for packet: %s\n", hit ? "Hit" : "Miss",
          pkt->print());

  if (hit) {
    // Respond to the CPU side
    stats.hits++; // update stats
    DDUMP(SetAssocCache, pkt->getConstPtr<uint8_t>(), pkt->getSize());
    pkt->makeResponse();
    sendResponse(pkt);
  } else {
    stats.misses++; // update stats
    missTime = curTick();
    // Forward to the memory side.
    // We can't fullyly forward the packet unless it is exactly the size
    // of the cache line, and aligned. Check for that here.
    Addr addr = pkt->getAddr();
    Addr block_addr = pkt->getBlockAddr(blockSize);
    unsigned size = pkt->getSize();
    if (addr == block_addr && size == blockSize) {
      // Aligned and block size. We can just forward.
      DPRINTF(SetAssocCache, "forwarding packet\n");
      memPort.sendPacket(pkt);
    } else {
      DPRINTF(SetAssocCache, "Upgrading packet to block size\n");
      panic_if(addr - block_addr + size > blockSize,
               "Cannot handle accesses that span multiple cache lines");
      // Unaligned access to one cache block
      assert(pkt->needsResponse());
      MemCmd cmd;
      if (pkt->isWrite() || pkt->isRead()) {
        // Read the data from memory to write into the block.
        // We'll write the data in the cache (i.e., a writeback cache)
        cmd = MemCmd::ReadReq;
      } else {
        panic("Unknown packet type in upgrade size");
      }

      // Create a new packet that is blockSize
      PacketPtr new_pkt = new Packet(pkt->req, cmd, blockSize);
      new_pkt->allocate();

      // Should now be block aligned
      assert(new_pkt->getAddr() == new_pkt->getBlockAddr(blockSize));

      // Save the old packet
      originalPacket = pkt;

      DPRINTF(SetAssocCache, "forwarding packet\n");
      memPort.sendPacket(new_pkt);
    }
  }
}

bool SetAssocCache::accessFunctional(PacketPtr pkt) {
  panic("TODO");
  return false;
}

void SetAssocCache::insert(PacketPtr pkt) {
  panic("TODO");
}

AddrRangeList SetAssocCache::getAddrRanges() const {
  DPRINTF(SetAssocCache, "Sending new ranges\n");
  // Just use the same ranges as whatever is on the memory side.
  return memPort.getAddrRanges();
}

void SetAssocCache::sendRangeChange() const {
  for (auto &port : cpuPorts) {
    port.sendRangeChange();
  }
}

SetAssocCache::SetAssocCacheStats::
    SetAssocCacheStats(statistics::Group *parent)
    : statistics::Group(parent),
      ADD_STAT(hits, statistics::units::Count::get(), "Number of hits"),
      ADD_STAT(misses, statistics::units::Count::get(), "Number of misses"),
      ADD_STAT(missLatency, statistics::units::Tick::get(),
               "Ticks for misses to the cache"),
      ADD_STAT(hitRatio, statistics::units::Ratio::get(),
               "The ratio of hits to the total accesses to the cache",
               hits / (hits + misses)) {
  missLatency.init(16); // number of buckets
}

} // namespace gem5
