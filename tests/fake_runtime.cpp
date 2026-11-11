// tests/fake_runtime.cpp — see fake_runtime.hpp.

#define VGX_FAKE_RUNTIME_BUILD
#include "fake_runtime.hpp"

namespace {
class Widget final : public IWidget {
public:
    int  __stdcall value(int x) override { return x + 1; }
    void __stdcall destroy() override { delete this; }
};
}

IWidget* vgx_make_widget() { return new Widget; }
int __stdcall vgx_alternate_value(IWidget*, int x) { return x + 2; }
