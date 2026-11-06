// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#pragma once

class UK2Node_VaRestRequestBase;
class FKismetCompilerContext;
class UEdGraph;
namespace VaRest::Http::Editor
{
void ExpandFullRequest(UK2Node_VaRestRequestBase& Node,FKismetCompilerContext& Compiler,UEdGraph* Graph);
}
