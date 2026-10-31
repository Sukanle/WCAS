# Controller：非侵入式接口与设备分离

WCAS 的控制器不再通过继承或虚函数区分设备，而是把「能力」定义为 ABIX Interface 契约，把「设备」定义为普通 provider。新增手柄、虚拟鼠标或其它输入后端时，只需要新增 provider 和它的方法表，不改已有接口，也不改调用方。

契约定义在 [`include/skl/wcas/controller.h`](../include/skl/wcas/controller.h)， provider 与适配器在 [`src/skl/wcas/harness/controller.cpp`](../src/skl/wcas/harness/controller.cpp)。

## 为什么选用 ABIX Interface

控制器后端的变化方向很明确：现在只有键盘鼠标（走私有 Wayland seat），以后会有手柄（可能走 uinput / libei），还可能出现由独立共享库提供的虚拟设备。这里需要的是一个 **稳定的能力契约**，而不是一棵继承树。选 ABIX Interface 主要基于下面几点。

### 1. 不侵入设备类型

ABIX 的 provider 是普通 C++ struct，不需要继承 `IController`，也不需要虚函数。键盘鼠标后端可以直接持有 `proxy::ConnState`，手柄后端可以持有设备 fd 或平台句柄；它们只在边界处提供「首参是对象句柄」的适配器：

```cpp
result<void> km_move(const void *self, cv::Point2d pos);
result<void> gp_button(const void *self, uint32_t button, bool pressed);
```

这带来两个实际好处：

- 设备类型可以自由使用值语义、聚合初始化或内部虚函数，不被接口基类绑死；
- 同一个设备可以同时实现多个能力（键盘鼠标实现 `PointerController` + `KeyboardController`），不再需要为“所有设备”设计一个越来越胖的基类。

### 2. 接口身份是内容哈希，不依赖 vtable 布局

C++ 虚函数的调用约定由编译器决定：vtable 顺序、`this` 调整、thunk、RTTI 都是实现细节。跨共享库、跨编译器或跨语言时，这些细节不能当作 ABI。

ABIX 的接口 id 是 **接口名 + 版本 + 有序方法签名** 的内容哈希，方法身份是 `(interface_id, slot)`。因此：

- 新增一台设备不会改变已有接口的 id；
- 接口版本不匹配时得到明确的 `version_mismatch`，而不是调用到错误的函数；
- 绑定阶段会逐个校验签名，不匹配时返回 `signature_mismatch` / `method_not_found`，不会把错误签名当成正确函数调用。

这也是为什么接口要按能力拆分：如果把手柄的 `axis` 追加到 `PointerController`，接口 id 会变化；旧的 provider 没有这个新 slot，绑定会失败，已有消费者也无法再按旧版本解析。正确做法是新增 `GamepadController`，或发布新版本并保留旧实现。

### 3. 绑定一次，调用是普通间接调用

`InterfaceRegistry` / `InterfaceRef` 是**绑定期**设施。`bind()` 或 `resolve()` 做一次查找并缓存适配器指针；之后每次 `move()` / `key()` 都是普通函数指针调用，不做名称比较、类型检查或注册表查找。ABIX 的测试用 `lookup_count()` 证明这一点。

这适合输入注入这种高频路径：分辨率、事件数量和窗口切换都可能很频繁，不能在每个事件上做一次动态查找。

### 4. 为后续的设备模块留出空间

如果手柄后端以后作为独立 `.so` 发布，ABIX 的 `ModuleManifest` + `ModuleRegistry` 可以让消费者在运行时发现它：

- provider 导出固定的 `abix_module_query_v1`，不需要解码 C++ 符号名；
- 消费者用 `InterfaceId` 查询，不依赖具体类名；
- 绑定持有一个 module lease，模块在函数指针仍被使用时不会被卸载；
- 同一个接口可以有多个实现，查询返回全部。

如果只用 C++ 虚基类，这些能力都要自己重新实现一套，而且很难保证跨编译器一致。

### 5. 失败是显式的

ABIX 提供返回 `InterfaceStatus` 的 checked 重载，`Controller` 门面也会先检查 `valid()`，因此不会因为 slot 没绑定而跳到空指针：

- 绑定失败有 `interface_not_found` / `version_mismatch` / `method_not_found`；
- `Controller` 门面在能力未实现时返回 `errc::feature_not_implemented`；
- `DeviceInfo::connected` 和 `capabilities` 让调用方在调用前就能判断。

对于自动化框架，显式失败比 UB 更重要：上层可以决定重试、切换后端或报告“不支持”。

### 与其它方案的对比

| 方案                     | 侵入性                  | ABI 稳定性                    | 版本/签名校验  | 运行时发现         | 失败语义            |
| ------------------------ | ----------------------- | ----------------------------- | -------------- | ------------------ | ------------------- |
| C++ 虚基类               | 必须继承、必须 override | vtable/布局随编译器和接口变化 | 无             | 无                 | 调错即 UB           |
| 手写函数指针表           | 无                      | 依赖人工约定 slot             | 无             | 需自己做           | 容易空指针/错签名   |
| `std::variant` + `visit` | 无                      | 进程内                        | 编译期         | 不支持后加载       | 编译期穷举          |
| C ABI + `dlsym`          | 无                      | 较稳定                        | 需自己做       | 需自己做           | 需自己做            |
| **ABIX Interface**       | **无**                  | **内容哈希 + 显式版本**       | **绑定期校验** | **ModuleRegistry** | **InterfaceStatus** |

### 代价与边界

ABIX Interface 不是“零成本”的：

- 每个方法要写适配器、方法表和注册代码，样板比虚函数多；本项目用 `SKL_ABIX_INTERFACE_DECLARE` 和集中的 `controller.h` 降低重复；
- slot 顺序是 ABI，必须遵守“只追加、不改序、变更走新版本”的纪律；
- 它不负责对象生命周期，provider 仍需由 `Controller::Impl`、`ControlledApp` 或模块 lease 保证存活；
- 当前接口仍使用 `result<void>`、`std::string` 等 C++ 类型，跨语言时应换成 POD。

如果项目永远只有一个进程内后端、也从不加载新设备，C++ 虚函数确实更简单。但 WCAS 的目标是键盘鼠标、手柄以及未来可能的模块化后端共存，ABIX Interface 用较小的声明成本换来了稳定的契约、显式的失败和可扩展的设备边界，因此这里是更合适的选择。

## 1. 接口拆分

| 契约                 | 方法                                                                 | 典型 provider      |
| -------------------- | -------------------------------------------------------------------- | ------------------ |
| `DeviceInfo`         | `id` / `kind` / `name` / `capabilities` / `connected`                | 所有 provider      |
| `PointerController`  | `move` / `click` / `scroll` / `abs_drag` / `rel_drag` / `press_drag` | 键盘鼠标、虚拟鼠标 |
| `KeyboardController` | `key` / `type_text`                                                  | 键盘、文本输入     |
| `GamepadController`  | `button` / `axis` / `rumble`                                         | 手柄               |

`DeviceInfo` 是通用接口：消费者不必知道具体设备类型，就能列出设备、判断连接状态，并通过 `capabilities` 位掩码决定是否可以调用指针/键盘/手柄方法。

一个 provider 可以实现多个契约。键盘鼠标同时注册 `DeviceInfo`、`PointerController` 和 `KeyboardController`；手柄只注册 `DeviceInfo` 和 `GamepadController`。

> 不要把新设备的方法追加到已有契约中。接口 id 由名称、版本和有序方法签名共同决定，追加或重排 slot 会改变 id，使已有绑定和模块失配。新增能力应新增契约，或以新版本发布。

## Proxy 存储：为什么选 Unique

ABIX 现在提供两种 proxy 布局：

| 布局                   | 每个 proxy 的大小 | 适配器存储                                  | 调用路径                 |
| ---------------------- | ----------------- | ------------------------------------------- | ------------------------ |
| `UniqueInterfaceProxy` | `24 + 8 * N`      | 内联在 proxy 中                             | proxy → 函数指针         |
| `SharedInterfaceProxy` | `24`              | 所有 proxy 共享一个 `InterfaceSlotTable<N>` | proxy → table → 函数指针 |

WCAS 的四个 Controller 契约在当前实现下的实测大小（Debug，x86-64）：

```text
unique:  DeviceInfo=64  Pointer=72  Keyboard=40  Gamepad=48  total=224 bytes
tables:  DeviceInfo=48  Pointer=56  Keyboard=24  Gamepad=32  total=160 bytes
shared:  每个 proxy 24 bytes
```

也就是说，**只创建一个 Controller 时**：

- Unique：224 bytes，没有额外 table；
- Shared：4 × 24 + 160 = 256 bytes，反而更大；
- Shared 只有在同一个 `(TypeId, InterfaceId, version)` 的 table 被**多个 proxy 共享** 时才划算。以 Pointer 为例：`72 vs 24 + 56/N`，N ≥ 2 时共享开始占优。

WCAS 的实际情况是：

- 每个被启动的应用创建一个 `Controller`，不是每个事件、每个窗口创建一个；
- 输入注入是热路径，Unique 少一次 `table` 间接寻址；
- Unique 不需要额外管理 table 的生命周期，也不会出现“proxy 比 table 活得久”的问题。

因此这里选择 **Unique**，四个契约都用 `SKL_ABIX_INTERFACE_UNIQUE_DECLARE`。如果以后出现“同一个实现对应大量 proxy” 的场景（例如每个窗口一个 controller、设备列表里成百上千个句柄），再考虑：

1. 每个 `(TypeId, InterfaceId, version)` 构建一个 `InterfaceSlotTable<N>`；
2. 把 table 放在 provider/registry 一侧，保证它比所有 proxy 活得久；
3. 用 `SKL_ABIX_INTERFACE_DECLARE_SHARED` 生成 `SharedXxx` 代理；
4. 模块发现路径下继续持有 module lease。

### Registry 也是共享的

`InterfaceRegistry<8>` 本身约 464 字节，但它是**绑定期**结构：只保存 `(TypeId, InterfaceId, version) -> 静态方法表`，不保存每个对象的状态。因此它不应该放进 `Controller::Impl`，否则每个 controller 都会多一份。

当前实现：

```cpp
// InterfaceRegistry 含 atomic 计数器，不可拷贝/移动，所以用 holder 原地构造。
struct ControllerRegistry {
    ControllerRegistry() {
        if (register_keyboard_mouse(registry) != InterfaceStatus::ok) return;
        // register_gamepad(registry);  // 以后新增
    }
    InterfaceRegistry<8> registry;
};

const InterfaceRegistry<8> &controller_registry() {
    static const ControllerRegistry holder;
    return holder.registry;
}
```

这样整个进程只有一份 registry（464 B），`Controller::create` 绑定完就只保留代理里的函数指针。以后新增后端，把注册调用加进 `ControllerRegistry` 的构造函数即可。

`InterfaceRegistry` **不做内部同步**：`add()` 必须由调用方串行化，并且不能与 `find()` 并发。这里的 registry 在函数内静态初始化时一次性填充，发布之后不再修改；因此多个线程同时 `Controller::create()` 执行 `find()` 是安全的，不需要额外的锁。诊断用的 lookup 计数器是 relaxed atomic，不会引入数据竞争。

如果以后要支持运行时动态注册 provider，应使用带模块生命周期/lease 管理的 `ModuleRegistry`，而不是往这个静态 registry 里并发写；`ModuleRegistry` 的注册调用同样需要调用方串行化。

## 2. 使用

```cpp
auto launched = harness.launch(config);
if (!launched) return 1;
auto app = std::move(launched.value());

auto created = app.controller();
if (!created) return 1;
auto controller = std::move(created.value());

if (controller.has_pointer())
    controller.click({100, 200});

if (controller.has_keyboard())
    controller.key("ctrl+s", std::chrono::milliseconds{50});

if (controller.has_gamepad())
    controller.axis(0, 0.5);

// 通用设备信息
fmt::println("device={} kind={} connected={} capabilities={:#x}", controller.name(),
    static_cast<int>(controller.kind()), controller.connected(), controller.capabilities());
```

未实现的能力返回 `errc::feature_not_implemented`，不会调用未绑定的 slot：

```cpp
if (auto result = controller.rumble(0.2, 0.8, std::chrono::seconds{1}); !result)
    fmt::println("{}", result.error().message());
```

## 3. 新增一个手柄 provider

推荐把适配器直接写成 provider 的成员函数，再用 `SKL_ABIX_INTERFACE_MEMBER_ENTRY` 生成方法表。这样不需要为每个 slot 手写一个 `const void*` 首参的免费函数，绑定代码也更集中。

```cpp
class Gamepad {
public:
    explicit Gamepad(int fd) : fd_(fd) {}

    // DeviceInfo
    // 与 MEMBER_ENTRY 一起使用时不要写 noexcept，见下方说明。
    DeviceKind kind() const { return DeviceKind::gamepad; }
    uint64_t id() const { return 1; }
    std::string name() const { return "xbox-gamepad"; }
    uint64_t capabilities() const {
        return device_capability_gamepad | device_capability_rumble;
    }
    bool connected() const { return fd_ >= 0; }

    // GamepadController
    result<void> button(uint32_t button, bool pressed) {
        // 写手柄设备（uinput / libei / 平台后端）
        return boost::outcome_v2::success();
    }
    result<void> axis(uint32_t axis, double value) { return boost::outcome_v2::success(); }
    result<void> rumble(double low, double high, std::chrono::milliseconds duration) {
        return boost::outcome_v2::success();
    }

private:
    int fd_ = -1;
};

const InterfaceMethod kGamepadInfo[] = {
    SKL_ABIX_INTERFACE_MEMBER_ENTRY(DeviceInfoTraits, id, &Gamepad::id),
    SKL_ABIX_INTERFACE_MEMBER_ENTRY(DeviceInfoTraits, kind, &Gamepad::kind),
    SKL_ABIX_INTERFACE_MEMBER_ENTRY(DeviceInfoTraits, name, &Gamepad::name),
    SKL_ABIX_INTERFACE_MEMBER_ENTRY(DeviceInfoTraits, capabilities, &Gamepad::capabilities),
    SKL_ABIX_INTERFACE_MEMBER_ENTRY(DeviceInfoTraits, connected, &Gamepad::connected),
};

const InterfaceMethod kGamepadMethods[] = {
    SKL_ABIX_INTERFACE_MEMBER_ENTRY(GamepadControllerTraits, button, &Gamepad::button),
    SKL_ABIX_INTERFACE_MEMBER_ENTRY(GamepadControllerTraits, axis, &Gamepad::axis),
    SKL_ABIX_INTERFACE_MEMBER_ENTRY(GamepadControllerTraits, rumble, &Gamepad::rumble),
};

constexpr auto kGamepadType =
    skl::abix::model::canonical_type_id("skl::wcas::Gamepad");

InterfaceStatus register_gamepad(InterfaceRegistry<8> &registry) {
    auto status = registry.add({DeviceInfoTraits::id, kGamepadType, kGamepadInfo,
        std::size(kGamepadInfo), DeviceInfoTraits::version, 0});
    if (status != InterfaceStatus::ok) return status;
    return registry.add({GamepadControllerTraits::id, kGamepadType, kGamepadMethods,
        std::size(kGamepadMethods), GamepadControllerTraits::version, 0});
}
```

`SKL_ABIX_INTERFACE_MEMBER_ENTRY` 内部使用 `member_adapter<&T::method>::invoke`，生成的函数形状仍然是：

```cpp
R invoke(const void *self, Args... args);
```

因此它正好匹配 `SKL_ABIX_INTERFACE_UNIQUE_DECLARE` 生成的 proxy。规则：

- 支持 `const` / 非 `const` 成员函数；
- **Clang/clangd 下避免给用于 `MEMBER_ENTRY` 的成员写 `noexcept`**：Clang 目前会把 `const` 和 `const noexcept` 两个 `member_adapter` 偏特化视为 ambiguous（GCC 可以接受）。成员不写 `noexcept` 仍然可以被 noexcept 调用点使用，适配器的函数类型也不受影响；
- 成员函数必须是可访问的（通常为 public），因为方法表在类外取成员指针；
- 返回类型和参数类型必须与接口契约完全一致；
- 不支持 `volatile`、引用限定成员（`&` / `&&`）和重载；重载需要显式转换为成员指针；
- `slot_name` 来自 `Traits::slots`，可以和成员函数名不同。

内置的 `KeyboardMouseDevice` 也采用了这种写法：`move` / `click` / `key` 等是它的成员函数，方法表只保留 slot 绑定，不再维护一组 `km_*` 免费函数。

然后在 `Controller::create` 中把 provider 放进 `Controller::Impl`，注册并绑定：

```cpp
struct Controller::Impl {
    Impl(ControlledApp &app, ControlledApp::Impl *state)
        : keyboard_mouse(&app, state) {}

    KeyboardMouseDevice keyboard_mouse;
    Gamepad pad;                 // 新增设备
    InterfaceRegistry<16> registry;
    // ... proxies ...
};

// Controller::create:
register_keyboard_mouse(impl->registry);
register_gamepad(impl->registry);                    // 新增
impl->gamepad.bind(impl->registry, &impl->pad, kGamepadType);
```

`Controller::Impl` 持有 provider，`Controller` 的代理只缓存函数指针；因此 provider 必须和 `Controller` 同生命周期。若以后要把设备做成真正的运行时可插拔后端，可以把这段绑定逻辑抽成 `Controller::bind(owner, object, type, registry)` 工厂。

## 4. 作为模块动态发现

如果手柄后端以后以独立共享库发布，可以让它导出 `ModuleManifest`，由 `ModuleRegistry` 发现：

```cpp
ModuleRegistry<> modules;
ModuleControl control;
control.name = "gamepad";
modules.register_module(control, manifest);

const auto *binding = modules.find_first(
    GamepadControllerTraits::id, GamepadControllerTraits::version);
auto bound = bind_interface(*binding, &pad);   // 持有 module lease

GamepadController gamepad;
gamepad.bind_methods(binding->interface_id, binding->version,
    binding->methods, binding->method_count, &pad);
```

lease 保证模块不会在缓存的函数指针仍被使用时卸载。模块卸载前会等待 lease 释放。

## 5. 注意事项

- **方法表必须是 `const InterfaceMethod[]`**，不能写成 `constexpr`：`interface_entry()` 不是 `constexpr`。
- **调用点参数类型必须与适配器一致**。`SKL_ABIX_INTERFACE_*` 宏从调用点推导 `std::decay_t<Args>...`；例如适配器参数是 `std::string` 时，传 `"ctrl+s"` 会推导成 `const char*`，编译不报错但运行错误。统一传 `std::string` 变量，复杂签名用 AMC 或手写 proxy。
- **provider 对象必须比 Controller 活得久**。当前 `Controller` 持有 `ControlledApp` 的内部状态指针，不能比 `ControlledApp` 活得更久，也不能在 `ControlledApp` 移动后继续使用旧 Controller。
- **接口必须实现全部 slot**。`InterfaceProxy` 绑定后会校验所有签名，缺一个 slot 整个代理就是 `valid() == false`。这也是按能力拆分接口的原因。
- **接口注册表容量固定**。`InterfaceRegistry<N>` 的 `N` 要覆盖所有 `(TypeId, InterfaceId)` 条目。
- **跨语言/跨编译器**：`result<void>`、`std::string`、`std::chrono::milliseconds` 是 C++ 类型。真正跨语言时应换成 POD 状态码和固定布局参数。

## 6. 相关文档

- [ABIX Interface 文档](https://github.com/Sukanle/ABIX/blob/v0.2.3/docs/abix/interface_zh.md)
- [ABIX 无 AMC 用法](https://github.com/Sukanle/ABIX/blob/v0.2.3/docs/abix/without_amc_zh.md)
