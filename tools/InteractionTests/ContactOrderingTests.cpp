// Contact ordering. These exist because the engine originally ordered CollisionInfo
// by raw pointer addresses, which made the order contacts are resolved in depend on
// heap layout - so two runs of the same binary with the same seed resolved the same
// contacts in different orders. Sequential-impulse resolution is order dependent, so
// that divergence propagates into the simulated state and, in the distributed system,
// into which side of a border an object finishes on.
//
// The property under test is not "some particular order" but "an order derived only
// from data the simulation itself defines", i.e. world IDs.

#include "TestHarness.h"

#include "GameWorld.h"
#include "GameObject.h"
#include "CollisionDetection.h"
#include "AABBVolume.h"

#include <set>
#include <vector>
#include <memory>

using namespace NCL;
using namespace NCL::CSC8503;

namespace {
	GameObject* MakeCube(GameWorld& world, const Vector3& position) {
		GameObject* cube = new GameObject(NoSpecialFeatures, "cube");
		cube->SetBoundingVolume((CollisionVolume*)new AABBVolume(Vector3(1, 1, 1)));
		cube->GetTransform().SetScale(Vector3(2, 2, 2)).SetPosition(position);
		cube->SetActive(true);
		world.AddGameObject(cube);
		return cube;
	}

	CollisionDetection::CollisionInfo Pair(GameObject* a, GameObject* b) {
		CollisionDetection::CollisionInfo info;
		info.a = a;
		info.b = b;
		return info;
	}
}

TEST(ContactOrderFollowsWorldIdNotAddress) {
	GameWorld world;
	// Allocated in ascending address order is not guaranteed, so the test asserts
	// against world IDs, which ARE guaranteed ascending by AddGameObject.
	GameObject* first = MakeCube(world, Vector3(0, 0, 0));
	GameObject* second = MakeCube(world, Vector3(5, 0, 0));
	GameObject* third = MakeCube(world, Vector3(10, 0, 0));

	CHECK(first->GetWorldID() < second->GetWorldID());
	CHECK(second->GetWorldID() < third->GetWorldID());

	std::set<CollisionDetection::CollisionInfo> contacts;
	contacts.insert(Pair(second, third));
	contacts.insert(Pair(first, third));
	contacts.insert(Pair(first, second));

	CHECK_EQ(3, (int)contacts.size());

	std::vector<int> orderA;
	std::vector<int> orderB;
	for (const auto& contact : contacts) {
		orderA.push_back(contact.a->GetWorldID());
		orderB.push_back(contact.b->GetWorldID());
	}

	// Lexicographic on (worldID a, worldID b): (0,1), (0,2), (1,2).
	CHECK_EQ(first->GetWorldID(), orderA[0]);
	CHECK_EQ(second->GetWorldID(), orderB[0]);
	CHECK_EQ(first->GetWorldID(), orderA[1]);
	CHECK_EQ(third->GetWorldID(), orderB[1]);
	CHECK_EQ(second->GetWorldID(), orderA[2]);
	CHECK_EQ(third->GetWorldID(), orderB[2]);
}

TEST(ContactOrderIsAStrictWeakOrdering) {
	GameWorld world;
	GameObject* a = MakeCube(world, Vector3(0, 0, 0));
	GameObject* b = MakeCube(world, Vector3(5, 0, 0));

	CollisionDetection::CollisionInfo ab = Pair(a, b);
	CollisionDetection::CollisionInfo ab2 = Pair(a, b);
	CollisionDetection::CollisionInfo ba = Pair(b, a);

	// Irreflexive.
	CHECK(!(ab < ab));
	// Equivalent pairs compare neither way round.
	CHECK(!(ab < ab2));
	CHECK(!(ab2 < ab));
	// Asymmetric on genuinely different pairs.
	CHECK(ab < ba);
	CHECK(!(ba < ab));
}

TEST(DistinctPairsAreNeverEquivalent) {
	// The old comparator collapsed a pair to `(size_t)a + ((size_t)b << 32)`, which
	// discards b's top bits and lets the addition carry between the halves, so two
	// distinct pairs could compare equivalent - and std::set decides equivalence
	// from operator< alone, silently dropping the second contact. World IDs are
	// unique, so no collision is possible.
	GameWorld world;
	std::vector<GameObject*> objects;
	for (int i = 0; i < 40; i++) {
		objects.push_back(MakeCube(world, Vector3((float)i, 0, 0)));
	}

	std::set<CollisionDetection::CollisionInfo> contacts;
	int inserted = 0;
	for (size_t i = 0; i < objects.size(); i++) {
		for (size_t j = i + 1; j < objects.size(); j++) {
			contacts.insert(Pair(objects[i], objects[j]));
			inserted++;
		}
	}
	CHECK_EQ(inserted, (int)contacts.size());
}

TEST(SamePairInsertedTwiceIsDeduped) {
	GameWorld world;
	GameObject* a = MakeCube(world, Vector3(0, 0, 0));
	GameObject* b = MakeCube(world, Vector3(5, 0, 0));

	std::set<CollisionDetection::CollisionInfo> contacts;
	contacts.insert(Pair(a, b));
	contacts.insert(Pair(a, b));
	CHECK_EQ(1, (int)contacts.size());
}

TEST(NullSidesDoNotCrashTheComparator) {
	// A default-constructed CollisionInfo has null sides. The comparator must not
	// dereference them - it is used by std::set, so an accidental default entry
	// would take the process down rather than report a bad contact.
	GameWorld world;
	GameObject* a = MakeCube(world, Vector3(0, 0, 0));

	CollisionDetection::CollisionInfo empty;
	CollisionDetection::CollisionInfo half = Pair(a, nullptr);
	CollisionDetection::CollisionInfo full = Pair(a, a);

	CHECK(empty < half);
	CHECK(half < full);
	CHECK(!(full < empty));
}
