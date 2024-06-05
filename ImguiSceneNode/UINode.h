#pragma once
#include <vector>

class UINode
{
public:
	UINode();
	virtual void Render();
	void AddChild(UINode* node);
protected:
	virtual void RenderNodeComponents();
	std::vector<UINode*> childNodes;
};
