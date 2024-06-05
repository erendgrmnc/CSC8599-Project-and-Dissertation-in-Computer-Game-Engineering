#include "UINode.h"

UINode::UINode() {
}

void UINode::Render() {

	RenderNodeComponents();

	for (const auto& childNode : childNodes) {
		childNode->Render();
	}
}

void UINode::AddChild(UINode* node) {
	childNodes.push_back(node);
}

void UINode::RenderNodeComponents() {

}
