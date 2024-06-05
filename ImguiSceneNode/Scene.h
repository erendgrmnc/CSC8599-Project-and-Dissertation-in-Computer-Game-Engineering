#pragma once
#include "UINode.h"

class Scene : public UINode
{
public:
	Scene();
	void Render() override;
};
