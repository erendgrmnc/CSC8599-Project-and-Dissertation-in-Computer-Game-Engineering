#include "TestHarness.h"

#include "NetworkBase.h"

// NetworkBase's constructor is protected, so reaching it needs a derived type.
// Nothing else is required here: the property under test is what the accessors do
// when no ENet host was ever created.
namespace {
	struct HostlessNetworkBase : public NetworkBase {
		HostlessNetworkBase() = default;
	};
}

// A game server that never received its start packet still prints its @@FINAL line -
// that failure mode is exactly what the line exists to make visible. If these
// accessors dereferenced netHandle they would crash on precisely those runs.
TEST(ByteCountersReadZeroWithoutAHost) {
	HostlessNetworkBase base;
	CHECK_EQ(base.GetTotalSentData(), 0u);
	CHECK_EQ(base.GetTotalSentPackets(), 0u);
	CHECK_EQ(base.GetTotalReceivedData(), 0u);
}
