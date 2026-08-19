#include "TestHarness.h"

#include "NetworkObject.h"

#include <iostream>

// E8 costs snapshot and halo traffic in BYTES, from counters that record COUNTS.
// The conversion factor is sizeof, taken here rather than hand-added from the struct
// declaration: padding and alignment are not visible in the source, and an error in
// this constant scales the entire bandwidth claim.
//
// The printed line is the deliverable. The checks exist so that a wire-format change
// altering a packet size fails a test rather than silently invalidating a published
// figure.

TEST(PacketSizesForBandwidthAccounting) {
	std::cout << "PACKETSIZE"
		<< " full=" << sizeof(NCL::CSC8503::FullPacket)
		<< " delta=" << sizeof(NCL::CSC8503::DeltaPacket)
		<< " haloEntry=" << sizeof(NCL::CSC8503::HaloObjectState)
		<< " haloHeader=" << (sizeof(NCL::CSC8503::HaloUpdatePacket)
			- sizeof(NCL::CSC8503::HaloObjectState)
				* NCL::CSC8503::HaloUpdatePacket::MAX_ENTRIES)
		<< "\n";

	// A delta must be smaller than a full snapshot, or the delta path costs more than
	// it saves and the 1 full : 5 delta cadence is a pessimisation.
	CHECK(sizeof(NCL::CSC8503::DeltaPacket) < sizeof(NCL::CSC8503::FullPacket));

	// The halo batch is sized to stay inside a typical 1400-byte MTU. Larger batches
	// are fragmented by ENet, which costs a retransmit of the whole packet if any one
	// fragment is lost.
	CHECK(sizeof(NCL::CSC8503::HaloUpdatePacket) <= 1400);
}
