#pragma once

namespace AZLua {
bool Init();
void Shutdown();
void Draw();
bool Run(const char* code);
const char* Version();
}
