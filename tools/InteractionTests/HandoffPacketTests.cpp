// Handoff packet layout, for the region-local world state increment.
//
// StartSimulatingObjectPacket gained mArchetypeID so a receiver that does not already
// hold the object can BUILD it rather than reject the handoff. Under the pre-seed
// model every server holds a deactivated twin of everything, so the field is inert -
// which is exactly why it needs a test now: nothing else exercises it until the
// pre-seed model is removed, and a silently wrong default would only surface then.

#include "TestHarness.h"

#include "GameObject.h"
#include "PhysicsObject.h"
#include "NetworkObject.h"
#include "NetworkState.h"
#include "AABBVolume.h"
#include "DistributedSystemCommonFiles/InteractionCommand.h"

#include <type_traits>

using namespace NCL;
using namespace NCL::CSC8503;
using namespace NCL::Interaction;

namespace {
	// A physics object is all StartSimulatingObjectPacket's constructor needs; the
	// packet reads velocity/force/inertia off it and the transform off that.
	struct Fixture {
		GameObject object{ NoSpecialFeatures, "handoff-cube" };

		Fixture() {
			object.SetBoundingVolume((CollisionVolume*)new AABBVolume(Vector3(1, 1, 1)));
			object.GetTransform().SetScale(Vector3(2, 2, 2)).SetPosition(Vector3(5, 0, 9));
			object.SetPhysicsObject(new PhysicsObject(&object.GetTransform(), object.GetBoundingVolume()));
			object.GetPhysicsObject()->SetInverseMass(1.0f);
			object.GetPhysicsObject()->InitCubeInertia();
		}
	};
}

TEST(HandoffPacketIsTriviallyCopyable) {
	// The ENet path memcpy's the struct, so a non-trivially-copyable member would be
	// undefined behaviour that happens to work on one compiler.
	//
	// This failed when written: NetworkState declared a virtual destructor while
	// nothing derived from it, so it carried a vtable pointer - and it is embedded by
	// value here AND in FullPacket, meaning every handoff and every full snapshot put
	// 8 bytes of process-local pointer on the wire.
	CHECK(std::is_trivially_copyable_v<NetworkState>);
	CHECK(std::is_trivially_copyable_v<StartSimulatingObjectPacket>);
	CHECK(std::is_trivially_copyable_v<FullPacket>);
}

TEST(NetworkStateHasNoVtablePointer) {
	// The direct form of the check above: a vptr shows up as size beyond the sum of
	// the members. Two Vector3, two Quaternion and an int, with no padding beyond
	// alignment on x64.
	constexpr size_t members = sizeof(Vector3) * 2 + sizeof(Quaternion) * 2 + sizeof(int);
	CHECK(sizeof(NetworkState) < members + sizeof(void*));
}

TEST(HandoffPacketReportsItsOwnSize) {
	Fixture fixture;
	NetworkState state;
	state.stateID = 3;

	StartSimulatingObjectPacket packet(42, 1, 0, state, *fixture.object.GetPhysicsObject());

	CHECK_EQ(packet.type, (short)BasicNetworkMessages::StartSimulatingObjectInServer);
	// size is the whole struct here, not the payload past GamePacket - matching the
	// constructor. Pinned so appending a field cannot silently truncate the send.
	CHECK_EQ((size_t)packet.size, sizeof(StartSimulatingObjectPacket));
}

TEST(HandoffPacketDefaultsToABuildableArchetype) {
	Fixture fixture;
	NetworkState state;

	StartSimulatingObjectPacket packet(42, 1, 0, state, *fixture.object.GetPhysicsObject());

	// Deliberately a real shape, not a sentinel: a receiver must always be able to
	// construct something. A handoff that arrived with an unbuildable archetype
	// would lose the object, which is worse than getting its shape wrong.
	CHECK_EQ(packet.mArchetypeID, (int)ObjectArchetype::Cube);
}

TEST(HandoffPacketCarriesArchetypeThroughACopy) {
	Fixture fixture;
	NetworkState state;

	StartSimulatingObjectPacket packet(42, 1, 0, state, *fixture.object.GetPhysicsObject());
	packet.mArchetypeID = (int)ObjectArchetype::Sphere;

	// The scheduled-handoff buffer copies the packet by value, so the field has to
	// survive that rather than only being correct at the send site.
	StartSimulatingObjectPacket copy = packet;
	CHECK_EQ(copy.mArchetypeID, (int)ObjectArchetype::Sphere);
	CHECK_EQ(copy.objectID, 42);
}

TEST(HandoffPacketAppendedFieldsDoNotDisturbEarlierOnes) {
	// The roles deploy separately, so every field added to this packet must be
	// APPENDED. This pins the earlier appended fields against a future insertion:
	// if mArchetypeID were inserted above them instead, these offsets would move.
	CHECK(offsetof(StartSimulatingObjectPacket, mControllerPlayerID)
		< offsetof(StartSimulatingObjectPacket, mSenderTick));
	CHECK(offsetof(StartSimulatingObjectPacket, mSenderTick)
		< offsetof(StartSimulatingObjectPacket, mArchetypeID));
	CHECK(offsetof(StartSimulatingObjectPacket, objectID)
		< offsetof(StartSimulatingObjectPacket, mControllerPlayerID));
}
