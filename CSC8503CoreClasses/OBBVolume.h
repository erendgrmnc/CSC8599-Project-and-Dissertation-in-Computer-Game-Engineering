#pragma once
#include "CollisionVolume.h"

namespace NCL {
	class OBBVolume : CollisionVolume
	{
	public:
		OBBVolume(const Maths::Vector3& halfDims, const Maths::Vector3& offset = Maths::Vector3(0, 0, 0)) {
			type		= VolumeType::OBB;
			halfSizes	= halfDims;
			// Was `this->offset = halfDims`, which ignored the parameter and displaced
			// every OBB by its own half-extents. CollisionDetection adds GetOffset()
			// to the world position on every OBB path, so that was a real shift.
			this->offset = offset;
			this->applyPhysics = true;
		}
		~OBBVolume() {}

		Maths::Vector3 GetHalfDimensions() const {
			return halfSizes;
		}
		Maths::Vector3 GetOffset() const override {
			return offset;
		}
	protected:
		Maths::Vector3 halfSizes;
		Maths::Vector3 offset;
	};
}

