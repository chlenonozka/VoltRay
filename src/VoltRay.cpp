#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <bluetoothapis.h>
#include <bluetoothleapis.h>
#include <hidsdi.h>
#include <hidpi.h>
#include <commdlg.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <dbt.h>
#include <mmsystem.h>
#include <mmdeviceapi.h>
#include <roapi.h>
#include <winstring.h>
#include <windows.devices.bluetooth.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <deque>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
constexpr GUID leInterface{0x781aee18,0x7733,0x4ce4,{0xad,0xd0,0x91,0xf4,0x1c,0x67,0xb5,0x92}};
constexpr GUID leServiceInterface{0x6e3bb679,0x4372,0x40c8,{0x9e,0xaa,0x45,0x09,0xdf,0x26,0x0c,0xd8}};
constexpr GUID batteryService{0x180f,0,0x1000,{0x80,0,0,0x80,0x5f,0x9b,0x34,0xfb}};
constexpr GUID batteryCharacteristic{0x2a19,0,0x1000,{0x80,0,0,0x80,0x5f,0x9b,0x34,0xfb}};
constexpr GUID batteryStatusCharacteristic{0x2bed,0,0x1000,{0x80,0,0,0x80,0x5f,0x9b,0x34,0xfb}};
constexpr DEVPROPKEY addressKey{{0x2bd67d8b,0x8beb,0x48d5,{0x87,0xe0,0x6c,0xda,0x34,0x28,0x04,0x0a}},1};
constexpr DEVPROPKEY batteryKey{{0x104ea319,0x6ee2,0x4701,{0xbd,0x47,0x8d,0xdb,0xf4,0x25,0xbb,0xe5}},2};
constexpr DEVPROPKEY containerKey{{0x8c7ed206,0x3f8a,0x4827,{0xb3,0xab,0xae,0x9e,0x1f,0xae,0xfc,0x6c}},2};
constexpr UINT trayMessage = WM_APP + 1;
constexpr UINT scanFinished = WM_APP + 2;
constexpr UINT soundFinished = WM_APP + 3;
constexpr UINT openFlyout = WM_APP + 4;
constexpr UINT_PTR pollTimer = 1;
constexpr UINT_PTR hotplugTimer = 2;
constexpr UINT activeRefreshMs = 60000;
constexpr UINT idleRefreshMs = 120000;
constexpr UINT flyoutRefreshMs = 5000;
constexpr int unknown = -1;
constexpr wchar_t settingsKey[] = L"Software\\PeripheralBatteryTray";
constexpr wchar_t trayWindowClass[] = L"PeripheralBatteryTrayWindow";
constexpr wchar_t appName[] = L"VoltRay";
constexpr int appIconResource = 1;
constexpr size_t slotCount = 4;
constexpr size_t keyboardSlot = 2;
constexpr size_t gamepadSlot = 3;
constexpr wchar_t uiFont[] = L"Segoe UI Variable Text";

enum class AppTheme : DWORD { system, light, dark };
AppTheme appTheme = AppTheme::system;

enum class AppLanguage : DWORD { English, Russian };
static AppLanguage g_language = AppLanguage::English;

const wchar_t* Text(const wchar_t* english, const wchar_t* russian) {
    return g_language == AppLanguage::Russian ? russian : english;
}

struct LocalizedText {
    const wchar_t* english;
    const wchar_t* russian;

    const wchar_t* get() const { return Text(english, russian); }
};

constexpr LocalizedText noConnectedDevices{L"No connected devices", L"Нет подключённых устройств"};
constexpr LocalizedText refreshLabel{L"Refresh", L"Обновить"};
constexpr LocalizedText checkingLabel{L"Checking…", L"Проверка…"};
constexpr LocalizedText lastReadingLabel{L"Last reading", L"Последний замер"};
constexpr LocalizedText notificationSoundLabel{L"Notification sound", L"Звук уведомления"};

enum class Connection { disconnected, other, usb, receiver, bluetooth };

const wchar_t* ConnectionLabel(Connection connection) {
    switch (connection) {
    case Connection::usb: return L"USB";
    case Connection::receiver: return Text(L"2.4 GHz", L"2,4 ГГц");
    case Connection::bluetooth: return L"Bluetooth";
    default: return L"";
    }
}

struct Device {
    std::wstring address;
    std::wstring name;
    std::wstring lePath;
    bool classicConnected = false;
    int kind = -1;
    int battery = unknown;
    std::vector<std::wstring> hidPaths;
    bool charging = false;
    bool receiverUnlinked = false;
    int invalidBluetoothLevel = unknown;
    Connection connection = Connection::disconnected;
    std::wstring container{};
    std::vector<std::wstring> alternateIds{};
    std::wstring alias{};
    std::wstring windowsName{};

    bool connected() const { return connection != Connection::disconnected; }
};

enum class Sound { notification, message, mail, reminder, custom };
struct AlertSettings {
    bool enabled = true;
    int threshold = 20;
    Sound sound = Sound::notification;
    std::wstring file;
};
struct BatteryRange {
    int from, to;
};
using BatteryRanges = std::array<BatteryRange, 3>;

constexpr BatteryRanges ContiguousBatteryRanges(int lowMax, int mediumMax) {
    return {{{0, lowMax}, {lowMax + 1, mediumMax}, {mediumMax + 1, 100}}};
}

constexpr DWORD themeChargingColor = 0x1000000; // Outside the 24-bit RGB range.

struct BatteryAppearance {
    BatteryRanges ranges = ContiguousBatteryRanges(20, 74);
    std::array<DWORD, 3> colors{0xff909c, 0xf0cc76, 0x83dda8}; // RGB
    DWORD chargingColor = themeChargingColor;
};

constexpr size_t InvalidBatteryRange(const BatteryRanges& ranges) {
    for (size_t i = 0; i < ranges.size(); ++i) {
        const auto& range = ranges[i];
        if (range.from < 0 || range.to > 100 || range.from > range.to) return i;
        for (size_t previous = 0; previous < i; ++previous) {
            if (range.from <= ranges[previous].to && ranges[previous].from <= range.to)
                return i;
        }
    }
    return ranges.size();
}

constexpr bool ValidBatteryRanges(const BatteryRanges& ranges) {
    return InvalidBatteryRange(ranges) == ranges.size();
}

constexpr size_t BatteryBand(const BatteryAppearance& appearance, int level) {
    for (size_t i = 0; i < appearance.ranges.size(); ++i) {
        const auto& range = appearance.ranges[i];
        if (range.from <= level && level <= range.to) return i;
    }
    return appearance.ranges.size();
}

struct BatteryImage {
    int level = unknown;
    bool charging = false;
    bool stale = false;

    bool operator!=(const BatteryImage& other) const {
        return level != other.level || charging != other.charging || stale != other.stale;
    }
};
struct LastSample {
    std::wstring identity;
    std::wstring name;
    Connection connection = Connection::disconnected;
    int level = unknown;

    bool operator==(const LastSample&) const = default;
    bool available() const { return !identity.empty(); }
};
struct Slot {
    LocalizedText label;
    const wchar_t* key;
    std::wstring choice{};
    AlertSettings alert{};
    BatteryAppearance appearance{};
    std::wstring alertDevice{};
    bool alreadyAlerted = false;
    bool numbers = false;
    bool inTray = true;
    BatteryImage shown{};
    LastSample lastSample{};
    HICON icon = nullptr;
    std::wstring tip{};
    bool registered = false;
};
std::array<Slot, slotCount> slots{{
    {{L"Mouse", L"Мышь"}, L"Mouse"},
    {{L"Headphones", L"Наушники"}, L"Headphones"},
    {{L"Keyboard", L"Клавиатура"}, L"Keyboard"},
    {{L"Gamepad", L"Геймпад"}, L"Gamepad"}
}};

std::wstring DeviceModelName(const Device& device) {
    if (!device.windowsName.empty()) return device.windowsName;
    return device.name != device.address ? device.name : std::wstring{};
}

std::wstring DefaultDeviceName(const Device& device) {
    auto name = DeviceModelName(device);
    if (!name.empty()) return name;
    if (device.kind >= 0 && static_cast<size_t>(device.kind) < slots.size())
        return slots[device.kind].label.get();
    return device.address;
}

std::wstring DisplayName(const Device& device) {
    return device.alias.empty() ? DefaultDeviceName(device) : device.alias;
}

using Choices = std::array<std::wstring, slotCount>;
struct Scan {
    HWND window;
    Choices choices;
    std::vector<Device> devices;
    std::array<size_t, slotCount> selected{};
};
std::vector<Device> listed;
bool scanning = false;
bool rescan = false;
UINT taskbarCreated = 0;

std::wstring Lower(std::wstring value) {
    if (!value.empty()) CharLowerBuffW(&value[0], static_cast<DWORD>(value.size()));
    return value;
}

std::wstring ContainerId(DEVINST node) {
    GUID id{};
    DEVPROPTYPE type = 0;
    ULONG bytes = sizeof(id);
    if (CM_Get_DevNode_PropertyW(node, &containerKey, &type,
            reinterpret_cast<PBYTE>(&id), &bytes, 0) != CR_SUCCESS || type != DEVPROP_TYPE_GUID ||
        bytes != sizeof(id) || IsEqualGUID(id, GUID{})) return {};
    wchar_t text[39]{};
    if (!StringFromGUID2(id, text, std::size(text))) return {};
    return Lower(text);
}

// The Device Object API is exported by cfgmgr32.dll. MinGW lacks its SDK declarations.
struct DeviceObjectKey {
    DEVPROPKEY key;
    ULONG store;
    PCWSTR locale;
};

struct DeviceObjectProperty {
    DeviceObjectKey key;
    DEVPROPTYPE type;
    ULONG size;
    const void* buffer;
};

std::wstring WindowsContainerName(const std::wstring& container) {
    if (container.empty()) return {};
    using GetProperties = HRESULT (WINAPI *)(ULONG, PCWSTR, ULONG, ULONG,
        const DeviceObjectKey*, ULONG*, const DeviceObjectProperty**);
    using FreeProperties = void (WINAPI *)(ULONG, const DeviceObjectProperty*);
    static const HMODULE configuration = GetModuleHandleW(L"cfgmgr32.dll");
    static const auto get = reinterpret_cast<GetProperties>(
        GetProcAddress(configuration, "DevGetObjectProperties"));
    static const auto free = reinterpret_cast<FreeProperties>(
        GetProcAddress(configuration, "DevFreeObjectProperties"));
    if (!get || !free) return {};

    constexpr ULONG deviceContainer = 2;
    // Query the system store to match the Windows Settings device list.
    constexpr DeviceObjectKey itemNameDisplay{
        {{0xb725f130,0x47ef,0x101a,{0xa5,0xf1,0x02,0x60,0x8c,0x9e,0xeb,0xac}},10},0,nullptr};
    ULONG count = 0;
    const DeviceObjectProperty* raw = nullptr;
    if (FAILED(get(deviceContainer, container.c_str(), 0, 1, &itemNameDisplay, &count, &raw))) return {};
    const auto release = [count](const DeviceObjectProperty* properties) { free(count, properties); };
    const std::unique_ptr<const DeviceObjectProperty, decltype(release)> properties(raw, release);
    if (count != 1 || !properties || raw->type != DEVPROP_TYPE_STRING || !raw->buffer ||
        raw->size < sizeof(wchar_t) || raw->size > 8192 || raw->size % sizeof(wchar_t) != 0) return {};

    const auto name = static_cast<PCWSTR>(raw->buffer);
    const auto end = name + raw->size / sizeof(wchar_t) - 1;
    return *end == L'\0' && std::find(name, end, L'\0') == end ? std::wstring(name, end) : std::wstring{};
}

std::wstring AddressOf(DEVINST node) {
    wchar_t address[13]{};
    ULONG bytes = sizeof(address);
    DEVPROPTYPE type = 0;
    if (CM_Get_DevNode_PropertyW(node, &addressKey, &type,
        reinterpret_cast<PBYTE>(address), &bytes, 0) == CR_SUCCESS &&
        type == DEVPROP_TYPE_STRING && bytes == sizeof(address) && address[12] == L'\0') {
        return Lower(address);
    }
    return {};
}

std::wstring AddressFromPath(const std::wstring& path) {
    // Service interfaces normally have a BTHLEDevice node with Dev_<address>.
    auto id = Lower(path);
    const auto start = id.find(L"dev_");
    if (start == std::wstring::npos || start + 16 > id.size()) return {};
    const auto address = id.substr(start + 4, 12);
    return address.find_first_not_of(L"0123456789abcdef") == std::wstring::npos ? address : L"";
}

Device& Upsert(std::vector<Device>& devices, const std::wstring& address) {
    for (auto& device : devices) if (device.address == address) return device;
    devices.push_back(Device{});
    devices.back().address = address;
    return devices.back();
}

int ClassifyName(const std::wstring& name) {
    const auto lower = Lower(name);
    if (lower.find(L"mouse") != lower.npos || lower.find(L"мыш") != lower.npos ||
        lower.find(L"phantom pro") != lower.npos ||
        lower.find(L"mx master") != lower.npos || lower.find(L"mx anywhere") != lower.npos) return 0;
    if (lower.find(L"headphone") != lower.npos || lower.find(L"headset") != lower.npos ||
        lower.find(L"g733") != lower.npos ||
        lower.find(L"airpods") != lower.npos || lower.find(L"buds") != lower.npos ||
        lower.find(L"наушник") != lower.npos || lower.find(L"гарнитур") != lower.npos) return 1;
    if (lower.find(L"keyboard") != lower.npos || lower.find(L"клавиатур") != lower.npos ||
        lower.find(L"mx keys") != lower.npos || lower.find(L"keychron") != lower.npos ||
        lower.find(L"kzzi") != lower.npos) return 2;
    for (const auto word : {L"gamepad", L"game controller", L"xbox wireless controller",
                           L"wireless controller", L"dualsense", L"joy-con", L"геймпад"}) {
        if (lower.find(word) != lower.npos) return gamepadSlot;
    }
    return -1;
}

void FindClassic(std::vector<Device>& devices) {
    BLUETOOTH_DEVICE_SEARCH_PARAMS search{};
    search.dwSize = sizeof(search);
    search.fReturnConnected = search.fReturnRemembered = search.fReturnAuthenticated = TRUE;
    BLUETOOTH_DEVICE_INFO info{};
    info.dwSize = sizeof(info);
    const auto finder = BluetoothFindFirstDevice(&search, &info);
    if (!finder) return;
    do {
        wchar_t address[13]{};
        swprintf_s(address, L"%012llx", static_cast<unsigned long long>(info.Address.ullLong));
        auto& device = Upsert(devices, address);
        device.name = info.szName;
        device.windowsName = device.name;
        device.classicConnected = info.fConnected != FALSE;
        if (device.classicConnected) device.connection = Connection::bluetooth;
        const auto major = (info.ulClassofDevice >> 8) & 31;
        const auto minor = (info.ulClassofDevice >> 2) & 63;
        if (major == 5 && (minor & 32)) device.kind = 0;
        else if (major == 5 && (minor & 16)) device.kind = 2;
        else if (major == 5 && ((minor & 15) == 1 || (minor & 15) == 2)) device.kind = gamepadSlot;
        else if (major == 4 && (minor == 1 || minor == 2 || minor == 6)) device.kind = 1;
    } while (BluetoothFindNextDevice(finder, &info));
    BluetoothFindDeviceClose(finder);
}

template<class Visitor>
void ForInterfaces(const GUID& guid, Visitor visit) {
    const auto set = SetupDiGetClassDevsW(&guid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return;
    for (DWORD index = 0; ; ++index) {
        SP_DEVICE_INTERFACE_DATA interfaceData{};
        interfaceData.cbSize = sizeof(interfaceData);
        if (!SetupDiEnumDeviceInterfaces(set, nullptr, &guid, index, &interfaceData)) break;
        DWORD size = 0;
        SetupDiGetDeviceInterfaceDetailW(set, &interfaceData, nullptr, 0, &size, nullptr);
        if (size < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W)) continue;
        std::vector<BYTE> storage(size);
        auto detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(storage.data());
        detail->cbSize = sizeof(*detail);
        SP_DEVINFO_DATA node{};
        node.cbSize = sizeof(node);
        if (SetupDiGetDeviceInterfaceDetailW(set, &interfaceData, detail, size, nullptr, &node))
            visit(set, node, std::wstring(detail->DevicePath));
    }
    SetupDiDestroyDeviceInfoList(set);
}

void FindLowEnergy(std::vector<Device>& devices) {
    ForInterfaces(leInterface, [&](HDEVINFO set, SP_DEVINFO_DATA& node, const std::wstring& path) {
        auto address = AddressOf(node.DevInst);
        if (address.empty()) address = AddressFromPath(path);
        if (address.empty()) return;
        auto& device = Upsert(devices, address);
        device.lePath = path;
        device.container = ContainerId(node.DevInst);
        if (!device.name.empty()) return;
        wchar_t name[256]{};
        if (SetupDiGetDeviceRegistryPropertyW(set, &node, SPDRP_FRIENDLYNAME,
                nullptr, reinterpret_cast<PBYTE>(name), sizeof(name), nullptr) ||
            SetupDiGetDeviceRegistryPropertyW(set, &node, SPDRP_DEVICEDESC,
                nullptr, reinterpret_cast<PBYTE>(name), sizeof(name), nullptr)) {
            name[255] = L'\0';
            device.name = name;
            device.windowsName = device.name;
        }
    });
}

void FindClassicBattery(const std::array<Device*, slotCount>& selected) {
    if (std::none_of(selected.begin(), selected.end(), [](const Device* device) {
        return device && device->classicConnected;
    })) return;
    constexpr ULONG flags = CM_GETIDLIST_FILTER_ENUMERATOR | CM_GETIDLIST_FILTER_PRESENT;
    ULONG length = 0;
    if (CM_Get_Device_ID_List_SizeW(&length, L"BTHENUM", flags) != CR_SUCCESS || length < 2) return;
    std::vector<wchar_t> ids(length);
    if (CM_Get_Device_ID_ListW(L"BTHENUM", ids.data(), length, flags) != CR_SUCCESS) return;
    for (auto id = ids.data(); *id; id += wcslen(id) + 1) {
        DEVINST node = 0;
        if (CM_Locate_DevNodeW(&node, id, CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS) continue;
        BYTE level = 0;
        ULONG bytes = sizeof(level);
        DEVPROPTYPE type = 0;
        if (CM_Get_DevNode_PropertyW(node, &batteryKey, &type, &level, &bytes, 0) != CR_SUCCESS ||
            type != DEVPROP_TYPE_BYTE || bytes != sizeof(level) || level > 100) continue;
        auto address = AddressOf(node);
        if (address.empty()) address = AddressFromPath(id);
        for (auto device : selected)
            if (device && device->classicConnected && device->address == address) device->battery = level;
    }
}

bool IsUuid(const BTH_LE_UUID& uuid, USHORT small, const GUID& full) {
    return uuid.IsShortUuid ? uuid.Value.ShortUuid == small : IsEqualGUID(uuid.Value.LongUuid, full);
}

struct BluetoothReading {
    int level = unknown;
    int invalidLevel = unknown;
    bool charging = false;
};

BluetoothReading DecodeBluetoothBatteryStatus(const BYTE* data, size_t length) {
    // Bluetooth SIG GATT Specification Supplement, Battery Level Status (0x2BED).
    if (length < 3 || (data[1] & 1) == 0) return {};
    const BYTE flags = data[0];
    const size_t levelOffset = (flags & 1) ? 5 : 3;
    const size_t expected = levelOffset + ((flags & 2) != 0) + ((flags & 4) != 0);
    if (length < expected) return {};
    BluetoothReading status;
    status.charging = (data[1] & 0x60) == 0x20;
    if (flags & 2) {
        const int raw = data[levelOffset];
        if (raw <= 100) status.level = raw;
        else status.invalidLevel = raw;
    }
    return status;
}

BluetoothReading ReadBatteryCharacteristic(HANDLE handle, BTH_LE_GATT_SERVICE* service) {
    USHORT count = 0;
    BluetoothGATTGetCharacteristics(handle, service, 0, nullptr, &count, BLUETOOTH_GATT_FLAG_NONE);
    if (!count) return {};
    std::vector<BTH_LE_GATT_CHARACTERISTIC> characteristics(count);
    if (FAILED(BluetoothGATTGetCharacteristics(handle, service, count, characteristics.data(),
            &count, BLUETOOTH_GATT_FLAG_NONE))) return {};
    BluetoothReading reading;
    BluetoothReading status;
    for (auto& characteristic : characteristics) {
        if (!characteristic.IsReadable) continue;
        const bool level = IsUuid(characteristic.CharacteristicUuid, 0x2a19, batteryCharacteristic);
        if (!level && !IsUuid(characteristic.CharacteristicUuid, 0x2bed, batteryStatusCharacteristic)) continue;
        struct {
            BTH_LE_GATT_CHARACTERISTIC_VALUE value;
            BYTE padding[8];
        } buffer{};
        if (FAILED(BluetoothGATTGetCharacteristicValue(handle, &characteristic, sizeof(buffer),
                &buffer.value, nullptr, BLUETOOTH_GATT_FLAG_FORCE_READ_FROM_DEVICE)) ||
            buffer.value.DataSize > sizeof(buffer) - offsetof(BTH_LE_GATT_CHARACTERISTIC_VALUE, Data)) continue;
        if (!level) {
            status = DecodeBluetoothBatteryStatus(buffer.value.Data, buffer.value.DataSize);
            continue;
        }
        if (buffer.value.DataSize != 1) continue;
        const int raw = buffer.value.Data[0];
        if (raw <= 100) reading.level = raw;
        else reading.invalidLevel = raw;
    }
    if (reading.level == unknown) reading.level = status.level;
    if (reading.invalidLevel == unknown) reading.invalidLevel = status.invalidLevel;
    reading.charging = status.charging;
    return reading;
}

struct CloseFile {
    void operator()(void* handle) const { CloseHandle(handle); }
};

enum class ReceiverProtocol { SplitCompX, DirectCompX, Logitech, CompXKeyboard };

constexpr wchar_t k75ModelName[] = L"KZZI K75 Pro";

struct ReceiverProfile {
    const wchar_t* usbId;
    const wchar_t* slotId;
    const wchar_t* name;
    int kind;
    ReceiverProtocol protocol;
    bool wireless = true;
};

constexpr ReceiverProfile receiverProfiles[] = {
    {L"vid_25a7&pid_fa7b", L"usb-mouse", L"ARDOR GAMING Phantom PRO", 0, ReceiverProtocol::SplitCompX, false},
    {L"vid_25a7&pid_fa7c", L"usb-mouse", L"ARDOR GAMING Phantom PRO", 0, ReceiverProtocol::SplitCompX},
    {L"vid_3554&pid_f52e", L"usb-mouse", L"ARDOR GAMING Phantom PRO V2", 0, ReceiverProtocol::DirectCompX, false},
    {L"vid_3554&pid_f52d", L"usb-mouse", L"ARDOR GAMING Phantom PRO V2", 0, ReceiverProtocol::DirectCompX},
    {L"vid_046d&pid_0afe", L"usb-headset", L"Logitech G733", 1, ReceiverProtocol::Logitech},
    {L"vid_046d&pid_0ab5", L"usb-headset", L"Logitech G733", 1, ReceiverProtocol::Logitech},
    {L"vid_046d&pid_0b1f", L"usb-headset", L"Logitech G733", 1, ReceiverProtocol::Logitech},
    {L"vid_3554&pid_fa09", L"usb-keyboard", L"KZZI", 2, ReceiverProtocol::CompXKeyboard}
};

bool MatchesReceiver(const std::wstring& lowerPath, const ReceiverProfile& profile) {
    // USB HID interface paths put the VID/PID immediately after the enumerator.
    return lowerPath.size() >= 26 && lowerPath.compare(0, 8, L"\\\\?\\hid#") == 0 &&
           lowerPath.compare(8, 17, profile.usbId) == 0 &&
           (lowerPath[25] == L'&' || lowerPath[25] == L'#');
}

using ReceiverHandle = std::unique_ptr<void, CloseFile>;

ReceiverHandle OpenReceiver(const std::wstring& path, DWORD access, DWORD flags) {
    const HANDLE handle = CreateFileW(path.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                      nullptr, OPEN_EXISTING, flags, nullptr);
    return ReceiverHandle(handle == INVALID_HANDLE_VALUE ? nullptr : handle);
}

bool ReceiverCaps(HANDLE handle, HIDP_CAPS& caps) {
    PHIDP_PREPARSED_DATA data = nullptr;
    if (!HidD_GetPreparsedData(handle, &data)) return false;
    const auto status = HidP_GetCaps(data, &caps);
    HidD_FreePreparsedData(data);
    return status == HIDP_STATUS_SUCCESS;
}

struct ReceiverEndpoint {
    std::wstring path;
    HIDP_CAPS caps{};
};

DWORD TimedReceiverIo(HANDLE handle, HANDLE event, bool write, BYTE* bytes, DWORD count, DWORD timeout) {
    OVERLAPPED operation{};
    operation.hEvent = event;
    ResetEvent(event);
    const BOOL started = write ? WriteFile(handle, bytes, count, nullptr, &operation)
                               : ReadFile(handle, bytes, count, nullptr, &operation);
    if (!started && GetLastError() != ERROR_IO_PENDING) return 0;
    DWORD transferred = 0;
    if (started || WaitForSingleObject(event, timeout) == WAIT_OBJECT_0) {
        if (GetOverlappedResult(handle, &operation, &transferred, FALSE)) return transferred;
    } else {
        CancelIoEx(handle, &operation);
        // Windows still owns both the report buffer and OVERLAPPED until I/O has completed.
        GetOverlappedResult(handle, &operation, &transferred, TRUE);
    }
    return 0;
}

enum class ReceiverLink { unknown, online, offline };

struct ReceiverBattery {
    int level = unknown;
    bool charging = false;
    ReceiverLink link = ReceiverLink::unknown;
    bool k75Pro = false;
};

constexpr DWORD receiverTimeoutMs = 450;
constexpr size_t compxFrameSize = 17;
constexpr BYTE compxLinkCommand = 0x03;
constexpr BYTE compxBatteryCommand = 0x04;

std::vector<BYTE> CompXRequest(size_t reportSize, BYTE command) {
    std::vector<BYTE> request(reportSize);
    request[0] = 0x08;
    request[1] = command;
    request[16] = static_cast<BYTE>(0x55 - request[0] - command);
    return request;
}

bool IsCompXReply(const BYTE* bytes, DWORD size, BYTE command) {
    if (size < compxFrameSize || (bytes[0] != 0x08 && bytes[0] != 0x09) ||
        bytes[1] != command || bytes[2] != 0) return false;
    unsigned sum = 0;
    for (size_t i = 0; i < compxFrameSize; ++i) sum += bytes[i];
    return (sum & 0xff) == 0x55;
}

ReceiverBattery DecodeCompXLink(const BYTE* bytes, DWORD size) {
    if (!IsCompXReply(bytes, size, compxLinkCommand) || bytes[5] != 1 || bytes[6] > 1) return {};
    return {unknown, false, bytes[6] ? ReceiverLink::online : ReceiverLink::offline};
}

ReceiverBattery DecodeCompXBattery(const BYTE* bytes, DWORD size) {
    if (!IsCompXReply(bytes, size, compxBatteryCommand) || bytes[6] > 100) return {};
    return {bytes[6], bytes[7] == 1, ReceiverLink::online};
}

int EstimateG733Battery(int voltage) {
    // G733 reports millivolts. Calibration used by LogiG733Tray.
    // https://github.com/Myrkie/LogiG733Tray/blob/mistress/LogiG733Tray/G733/G733Device.cs
    constexpr std::array<int, 8> volts{3500, 3530, 3659, 3690, 3725, 3800, 3870, 3990};
    constexpr std::array<int, 8> levels{0, 5, 10, 20, 40, 60, 80, 100};
    if (voltage <= volts.front()) return 0;
    for (size_t i = 1; i < volts.size(); ++i) {
        if (voltage > volts[i]) continue;
        return levels[i - 1] + MulDiv(voltage - volts[i - 1], levels[i] - levels[i - 1],
                                      volts[i] - volts[i - 1]);
    }
    return 100;
}

template <class Decode>
ReceiverBattery ReadReceiverReply(HANDLE input, HANDLE event, USHORT length, Decode decode,
                                  DWORD timeout = receiverTimeoutMs) {
    std::vector<BYTE> response(length);
    const auto deadline = GetTickCount64() + timeout;
    for (auto now = GetTickCount64(); now < deadline; now = GetTickCount64()) {
        const DWORD got = TimedReceiverIo(input, event, false, response.data(), length,
                                          static_cast<DWORD>(deadline - now));
        if (!got) return {};
        const auto reading = decode(response.data(), got);
        if (reading.link != ReceiverLink::unknown) return reading;
    }
    return {};
}

// ARDOR read-only battery framing. A receiver can cache its last percentage,
// so first require the live CompX connection query when operating over 2.4 GHz.
// https://github.com/KhromotozzDevOut/ardor-mouse-battery-tray
// https://github.com/HeyOkay/HaloBattery/blob/main/docs/protocols.md#irocks-m31r
ReceiverBattery QuerySplitCompX(const std::vector<ReceiverEndpoint>& endpoints, bool wireless) {
    const ReceiverEndpoint* input = nullptr;
    const ReceiverEndpoint* feature = nullptr;
    for (const auto& endpoint : endpoints) {
        const auto& caps = endpoint.caps;
        if (caps.UsagePage == 0xff01 && caps.InputReportByteLength >= compxFrameSize)
            input = &endpoint;
        if (caps.UsagePage == 0xff02 && caps.FeatureReportByteLength >= compxFrameSize)
            feature = &endpoint;
    }
    if (!input || !feature) return {};
    const auto inputFile = OpenReceiver(input->path, GENERIC_READ, FILE_FLAG_OVERLAPPED);
    const auto featureFile = OpenReceiver(feature->path, GENERIC_READ | GENERIC_WRITE, 0);
    const ReceiverHandle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!inputFile || !featureFile || !event) return {};
    const auto query = [&](BYTE command, auto decode) -> ReceiverBattery {
        auto request = CompXRequest(feature->caps.FeatureReportByteLength, command);
        if (!HidD_SetFeature(featureFile.get(), request.data(), static_cast<ULONG>(request.size()))) return {};
        return ReadReceiverReply(inputFile.get(), event.get(), input->caps.InputReportByteLength, decode);
    };
    if (!wireless) return query(compxBatteryCommand, DecodeCompXBattery);
    const auto status = query(compxLinkCommand, DecodeCompXLink);
    if (status.link != ReceiverLink::online) return status;
    const auto battery = query(compxBatteryCommand, DecodeCompXBattery);
    return battery.level == unknown ? status : battery;
}

ReceiverBattery QueryDirectCompX(const std::vector<ReceiverEndpoint>& endpoints, bool wireless) {
    for (const auto& endpoint : endpoints) {
        const auto& caps = endpoint.caps;
        if (caps.UsagePage != 0xff02 || caps.Usage != 0x0002 ||
            caps.InputReportByteLength < compxFrameSize || caps.OutputReportByteLength < compxFrameSize) continue;
        const auto file = OpenReceiver(endpoint.path, GENERIC_READ | GENERIC_WRITE, FILE_FLAG_OVERLAPPED);
        const ReceiverHandle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (!file || !event) continue;
        const auto query = [&](BYTE command, auto decode) -> ReceiverBattery {
            auto request = CompXRequest(caps.OutputReportByteLength, command);
            if (TimedReceiverIo(file.get(), event.get(), true, request.data(),
                                caps.OutputReportByteLength, receiverTimeoutMs) != request.size()) return {};
            return ReadReceiverReply(file.get(), event.get(), caps.InputReportByteLength, decode);
        };
        if (!wireless) {
            const auto reading = query(compxBatteryCommand, DecodeCompXBattery);
            if (reading.level != unknown) return reading;
            continue;
        }
        const auto status = query(compxLinkCommand, DecodeCompXLink);
        if (status.link == ReceiverLink::offline) return status;
        if (status.link != ReceiverLink::online) continue;
        const auto battery = query(compxBatteryCommand, DecodeCompXBattery);
        return battery.level == unknown ? status : battery;
    }
    return {};
}

// HID++ GetFeature (root, fn 0), followed by ADC measurement (feature 0x1F20, fn 0).
// https://openlogi.org/hidpp/features/x1f20-adc-measurement
struct G733Reply {
    bool success = false;
    BYTE error = 0;
    std::array<BYTE, 3> data{};
};

G733Reply ExchangeG733(HANDLE file, HANDLE event, const HIDP_CAPS& caps,
                       BYTE feature, std::array<BYTE, 3> parameters) {
    constexpr BYTE softwareId = 0x0a;
    constexpr DWORD replyTimeout = 900;
    std::vector<BYTE> request(caps.OutputReportByteLength);
    request[0] = 0x11;
    request[1] = 0xff;
    request[2] = feature;
    request[3] = softwareId; // Function 0, with a nonzero tag to identify solicited replies.
    std::copy(parameters.begin(), parameters.end(), request.begin() + 4);
    if (TimedReceiverIo(file, event, true, request.data(), caps.OutputReportByteLength,
                        receiverTimeoutMs) != request.size()) return {};

    std::vector<BYTE> reply(caps.InputReportByteLength);
    const auto deadline = GetTickCount64() + replyTimeout;
    for (auto now = GetTickCount64(); now < deadline; now = GetTickCount64()) {
        const auto size = TimedReceiverIo(file, event, false, reply.data(),
            caps.InputReportByteLength, static_cast<DWORD>(deadline - now));
        if (!size) break;
        if (size < 7 || (reply[0] != 0x11 && reply[0] != 0x10) || reply[1] != 0xff) continue;
        if (reply[2] == feature && reply[3] == softwareId)
            return {true, 0, {reply[4], reply[5], reply[6]}};
        if (reply[2] == 0xff && reply[3] == feature && reply[4] == softwareId)
            return {false, reply[5], {}};
    }
    return {};
}

ReceiverBattery DecodeG733Battery(const G733Reply& reply) {
    if (!reply.success)
        return {unknown, false, reply.error == 0x05 ? ReceiverLink::offline : ReceiverLink::unknown};
    const BYTE status = reply.data[2];
    if ((status & 0x01) == 0) return {unknown, false, ReceiverLink::offline};
    if (status == 0x07) return {100, false, ReceiverLink::online};
    if (status != 0x01 && status != 0x03) return {unknown, false, ReceiverLink::online};
    const int voltage = (reply.data[0] << 8) | reply.data[1];
    if (voltage < 3000 || voltage > 4500) return {unknown, false, ReceiverLink::online};
    return {EstimateG733Battery(voltage), status == 0x03, ReceiverLink::online};
}

ReceiverBattery QueryG733(const std::vector<ReceiverEndpoint>& endpoints) {
    for (const auto& endpoint : endpoints) {
        const auto& caps = endpoint.caps;
        if (caps.UsagePage != 0xff43 || caps.Usage != 0x0202 ||
            caps.InputReportByteLength < 7 || caps.OutputReportByteLength < 20) continue;
        const auto file = OpenReceiver(endpoint.path, GENERIC_READ | GENERIC_WRITE, FILE_FLAG_OVERLAPPED);
        const ReceiverHandle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (!file || !event) continue;
        const auto adcFeature = ExchangeG733(file.get(), event.get(), caps, 0, {0x1f, 0x20, 0});
        if (adcFeature.error == 0x05) return {unknown, false, ReceiverLink::offline};
        if (!adcFeature.success || !adcFeature.data[0] || adcFeature.data[0] == 0xff) continue;
        const auto reading = DecodeG733Battery(ExchangeG733(file.get(), event.get(), caps,
                                                           adcFeature.data[0], {}));
        if (reading.link != ReceiverLink::unknown) return reading;
    }
    return {};
}

bool IsWirelessKeyboardReply(const BYTE* bytes, DWORD size) {
    if (size != 20 || bytes[0] != 0x13) return false;
    unsigned sum = 0;
    for (size_t i = 0; i < 19; ++i) sum += bytes[i];
    return (sum & 0xff) == bytes[19];
}

ReceiverBattery DecodeWirelessKeyboardLink(const BYTE* bytes, DWORD size) {
    if (!IsWirelessKeyboardReply(bytes, size) ||
        bytes[1] != 0x07 || bytes[2] != 1 || bytes[3] != 0 || bytes[4] != 1) return {};
    return {unknown, false, bytes[5] ? ReceiverLink::online : ReceiverLink::offline};
}

ReceiverBattery DecodeWirelessKeyboardBattery(const BYTE* bytes, DWORD size) {
    if (!IsWirelessKeyboardReply(bytes, size)) return {};
    const bool solicited = (bytes[1] & 0x7f) == 0x4a && bytes[2] == 1 && bytes[3] == 0 && bytes[4] == 2;
    // BY wireless keyboards can also emit a power-status notification (type 0x05).
    const bool notification = bytes[1] == 0x0a && bytes[5] == 0x05;
    if (!solicited && !notification) return {};
    const BYTE level = bytes[notification ? 6 : 5];
    const BYTE flags = bytes[notification ? 7 : 6];
    if (level == 0 || level > 100 || (flags & 0xee) != 0) return {};
    return {level, (flags & 0x10) != 0, ReceiverLink::online};
}

ReceiverBattery DecodeK75ProBattery(const BYTE* bytes, DWORD size) {
    // The K75 Pro firmware extends its model reply with gauge telemetry.
    constexpr std::array<BYTE, 12> k75Model{0x05, 1, 0, 0x0a, 0x03, 0, 0, 0, 0, 0x67, 0, 1};
    if (!IsWirelessKeyboardReply(bytes, size) ||
        !std::equal(k75Model.begin(), k75Model.end(), bytes + 1) || bytes[13] != 0xa7) return {};
    const BYTE level = bytes[14];
    // Zero is legacy padding. 0x01 marks the status as available and 0x03 also marks active charging.
    const bool charging = bytes[15] == 0x03;
    return {level <= 100 ? level : unknown, charging, ReceiverLink::online, true};
}

ReceiverBattery QueryWirelessKeyboard(const std::vector<ReceiverEndpoint>& endpoints) {
    for (const auto& endpoint : endpoints) {
        const auto& caps = endpoint.caps;
        if (caps.UsagePage != 0xff02 || caps.Usage != 0x0002 ||
            caps.InputReportByteLength != 20 || caps.OutputReportByteLength != 20) continue;
        const auto file = OpenReceiver(endpoint.path, GENERIC_READ | GENERIC_WRITE, FILE_FLAG_OVERLAPPED);
        const ReceiverHandle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (!file || !event) continue;
        const auto query = [&](BYTE command, BYTE packetCount, auto decode, DWORD timeout) -> ReceiverBattery {
            std::array<BYTE, 20> request{0x13, command, packetCount};
            request.back() = static_cast<BYTE>(request[0] + command + packetCount);
            if (TimedReceiverIo(file.get(), event.get(), true, request.data(),
                                caps.OutputReportByteLength, receiverTimeoutMs) != request.size()) return {};
            return ReadReceiverReply(file.get(), event.get(), caps.InputReportByteLength, decode, timeout);
        };
        // KZZI's WebHID driver uses 0x07 and checks the live link byte.
        // https://drive3.kzzi.com/
        const auto status = query(0x07, 1, DecodeWirelessKeyboardLink, receiverTimeoutMs);
        if (status.link == ReceiverLink::offline) return status;
        if (status.link != ReceiverLink::online) continue;
        const auto k75 = query(0x05, 1, DecodeK75ProBattery, receiverTimeoutMs);
        if (k75.link == ReceiverLink::online) return k75;
        // Compx receivers share an ID across keyboard firmware. Only accept a
        // checksummed battery reply when the K75 telemetry extension is absent.
        // https://github.com/ushineko/sanshoku/issues/36
        // https://github.com/arutkowski00/mchose-hid-bridge
        const auto decodeBattery = [](const BYTE* bytes, DWORD size) {
            // A model reply can arrive while the receiver is handling the fallback query.
            const auto model = DecodeK75ProBattery(bytes, size);
            return model.k75Pro ? model : DecodeWirelessKeyboardBattery(bytes, size);
        };
        for (const BYTE packetCount : {BYTE{1}, BYTE{0}}) {
            const auto reading = query(0x4a, packetCount, decodeBattery, 900);
            if (reading.link == ReceiverLink::online) return reading;
        }
        return status;
    }
    return {};
}

void ReadReceiverBattery(Device& device) {
    const bool wired = device.connection == Connection::usb;
    device.receiverUnlinked = false;
    device.battery = unknown;
    device.charging = false;
    for (const auto& profile : receiverProfiles) {
        // The USB path must have been enumerated as a functional USB input first.
        // A receiver can keep answering even after the device switches to USB.
        if (device.address != profile.slotId || profile.wireless == wired) continue;
        std::wstring productName;
        std::vector<ReceiverEndpoint> endpoints;
        for (const auto& path : device.hidPaths) {
            if (!MatchesReceiver(Lower(path), profile)) continue;
            const auto metadata = OpenReceiver(path, 0, 0);
            if (!metadata) continue;
            if (device.windowsName.empty() && productName.empty()) {
                wchar_t product[256]{};
                if (HidD_GetProductString(metadata.get(), product, sizeof(product)))
                    productName = product;
            }
            HIDP_CAPS caps{};
            if (ReceiverCaps(metadata.get(), caps) && caps.UsagePage >= 0xff00)
                endpoints.push_back({path, caps});
        }
        if (endpoints.empty()) continue;
        ReceiverBattery reading;
        switch (profile.protocol) {
        case ReceiverProtocol::SplitCompX: reading = QuerySplitCompX(endpoints, profile.wireless); break;
        case ReceiverProtocol::DirectCompX: reading = QueryDirectCompX(endpoints, profile.wireless); break;
        case ReceiverProtocol::Logitech: reading = QueryG733(endpoints); break;
        case ReceiverProtocol::CompXKeyboard: reading = QueryWirelessKeyboard(endpoints); break;
        }
        if (!wired && reading.link == ReceiverLink::offline) {
            device.receiverUnlinked = true;
        }
        if (reading.link != ReceiverLink::online) continue;
        device.connection = wired ? Connection::usb : Connection::receiver;
        device.receiverUnlinked = false;
        device.battery = reading.level;
        device.charging = reading.charging;
        device.name = reading.k75Pro ? k75ModelName : profile.name;
        if (!wired) device.windowsName = std::move(productName);
        return;
    }
}
struct ReleaseInterface {
    template<class T> void operator()(T* pointer) const { pointer->Release(); }
};
template<class T> using Interface = std::unique_ptr<T, ReleaseInterface>;

struct WinRtApartment {
    HRESULT status = RoInitialize(RO_INIT_MULTITHREADED);
    ~WinRtApartment() { if (SUCCEEDED(status)) RoUninitialize(); }
};

void MarkBluetoothConnections(std::vector<Device>& devices) {
    using namespace ABI::Windows::Devices::Bluetooth;
    using namespace ABI::Windows::Foundation;

    if (std::none_of(devices.begin(), devices.end(), [](const Device& device) {
        return !device.lePath.empty();
    })) return;
    const WinRtApartment apartment;
    if (FAILED(apartment.status)) return;
    constexpr wchar_t className[] = L"Windows.Devices.Bluetooth.BluetoothLEDevice";
    HSTRING_HEADER classStorage{};
    HSTRING name = nullptr;
    if (FAILED(WindowsCreateStringReference(className, std::size(className) - 1, &classStorage, &name))) return;
    IBluetoothLEDeviceStatics* rawFactory = nullptr;
    if (FAILED(RoGetActivationFactory(name, __uuidof(IBluetoothLEDeviceStatics),
              reinterpret_cast<void**>(&rawFactory)))) return;
    const Interface<IBluetoothLEDeviceStatics> factory(rawFactory);

    for (auto& device : devices) {
        if (device.lePath.empty() || device.address.size() != 12 ||
            device.address.find_first_not_of(L"0123456789abcdef") != std::wstring::npos) continue;
        // Opening the Windows representation alone does not initiate a BLE connection.
        IAsyncOperation<BluetoothLEDevice*>* rawOperation = nullptr;
        if (FAILED(factory->FromBluetoothAddressAsync(wcstoull(device.address.c_str(), nullptr, 16),
                                                       &rawOperation))) continue;
        const Interface<IAsyncOperation<BluetoothLEDevice*>> operation(rawOperation);
        IAsyncInfo* rawState = nullptr;
        if (FAILED(operation->QueryInterface(__uuidof(IAsyncInfo), reinterpret_cast<void**>(&rawState))))
            continue;
        const Interface<IAsyncInfo> state(rawState);
        AsyncStatus status = AsyncStatus::Started;
        constexpr DWORD openTimeout = 1200;
        const auto deadline = GetTickCount64() + openTimeout;
        while (SUCCEEDED(state->get_Status(&status)) && status == AsyncStatus::Started &&
               GetTickCount64() < deadline) Sleep(15);
        if (status != AsyncStatus::Completed) {
            if (status == AsyncStatus::Started) state->Cancel();
            continue;
        }

        IBluetoothLEDevice* rawBluetooth = nullptr;
        if (FAILED(operation->GetResults(&rawBluetooth)) || !rawBluetooth) continue;
        const Interface<IBluetoothLEDevice> bluetooth(rawBluetooth);
        BluetoothConnectionStatus connection{};
        if (SUCCEEDED(bluetooth->get_ConnectionStatus(&connection)) &&
            connection == BluetoothConnectionStatus::BluetoothConnectionStatus_Connected)
            device.connection = Connection::bluetooth;
    }
}

int HidDeviceKind(HDEVINFO set, SP_DEVINFO_DATA& info, HANDLE handle);

void ClassifyBluetoothHid(std::vector<Device>& devices) {
    constexpr GUID mouseClass{0x4d36e96f,0xe325,0x11ce,{0xbf,0xc1,0x08,0x00,0x2b,0xe1,0x03,0x18}};
    constexpr GUID keyboardClass{0x4d36e96b,0xe325,0x11ce,{0xbf,0xc1,0x08,0x00,0x2b,0xe1,0x03,0x18}};
    if (std::none_of(devices.begin(), devices.end(), [](const Device& device) {
        return device.connection == Connection::bluetooth && device.kind < 0 && !device.container.empty();
    })) return;
    GUID hidInterface{};
    HidD_GetHidGuid(&hidInterface);
    ForInterfaces(hidInterface, [&](HDEVINFO set, SP_DEVINFO_DATA& info, const std::wstring& path) {
        const auto container = ContainerId(info.DevInst);
        if (container.empty()) return;
        const auto match = std::find_if(devices.begin(), devices.end(), [&](const Device& device) {
            return device.connection == Connection::bluetooth && device.kind < 0 && device.container == container;
        });
        if (match == devices.end()) return;
        const auto handle = OpenReceiver(path, 0, 0);
        if (HidDeviceKind(set, info, handle.get()) == static_cast<int>(gamepadSlot))
            match->kind = static_cast<int>(gamepadSlot);
    });
    for (const auto& entry : {std::pair{mouseClass, 0}, std::pair{keyboardClass, 2}}) {
        const auto set = SetupDiGetClassDevsW(&entry.first, nullptr, nullptr, DIGCF_PRESENT);
        if (set == INVALID_HANDLE_VALUE) continue;
        for (DWORD index = 0; ; ++index) {
            SP_DEVINFO_DATA info{};
            info.cbSize = sizeof(info);
            if (!SetupDiEnumDeviceInfo(set, index, &info)) break;
            const auto container = ContainerId(info.DevInst);
            if (container.empty()) continue;
            for (auto& device : devices) {
                if (device.connection == Connection::bluetooth && device.kind < 0 && device.container == container)
                    device.kind = entry.second;
            }
        }
        SetupDiDestroyDeviceInfoList(set);
    }
}
constexpr DEVPROPKEY busProductKey{{0x540b947e,0x8b40,0x45bc,{0xa8,0xa2,0x6a,0x0b,0x89,0x4c,0xbd,0xa2}},4};

std::wstring ConnectedPnpId(DEVINST node) {
    wchar_t value[MAX_DEVICE_ID_LEN]{};
    return CM_Get_Device_IDW(node, value, std::size(value), 0) == CR_SUCCESS ? Lower(value) : L"";
}

std::wstring UsbBusProduct(DEVINST node) {
    wchar_t value[256]{};
    ULONG size = sizeof(value);
    DEVPROPTYPE type = 0;
    if (CM_Get_DevNode_PropertyW(node, &busProductKey, &type, reinterpret_cast<PBYTE>(value), &size, 0)
            != CR_SUCCESS || type != DEVPROP_TYPE_STRING) return {};
    value[std::size(value) - 1] = L'\0';
    return value;
}

bool IsReceiverDescription(const std::wstring& name) {
    const auto lower = Lower(name);
    return lower.find(L"receiver") != lower.npos || lower.find(L"dongle") != lower.npos ||
           lower.find(L"приёмник") != lower.npos || lower.find(L"приемник") != lower.npos;
}

bool HasUsbId(const std::wstring& usbId, const ReceiverProfile& profile) {
    return usbId.size() >= 22 && usbId.compare(0, 4, L"usb\\") == 0 &&
           usbId.compare(4, 17, profile.usbId) == 0 &&
           (usbId[21] == L'\\' || usbId[21] == L'&');
}

int UsbBootKind(DEVINST usbNode) {
    wchar_t ids[1024]{};
    ULONG size = sizeof(ids);
    ULONG type = 0;
    if (CM_Get_DevNode_Registry_PropertyW(usbNode, CM_DRP_COMPATIBLEIDS, &type, ids, &size, 0)
            != CR_SUCCESS || type != REG_MULTI_SZ) return -1;
    for (size_t start = 0; start < std::size(ids) && ids[start];) {
        size_t end = start;
        while (end < std::size(ids) && ids[end]) ++end;
        const auto compatible = Lower(std::wstring(ids + start, end - start));
        if (compatible.find(L"class_03&subclass_01&prot_02") != compatible.npos) return 0;
        if (compatible.find(L"class_03&subclass_01&prot_01") != compatible.npos) return 2;
        start = end + 1;
    }
    return -1;
}

int HidDeviceKind(HDEVINFO set, SP_DEVINFO_DATA& info, HANDLE handle) {
    HIDP_CAPS caps{};
    if (handle && ReceiverCaps(handle, caps)) {
        if (caps.UsagePage == 0x01 && caps.Usage == 0x02) return 0;
        if (caps.UsagePage == 0x01 && caps.Usage == 0x06) return 2;
        if (caps.UsagePage == 0x01 && (caps.Usage == 0x04 || caps.Usage == 0x05)) return gamepadSlot;
        return -1;
    }

    // Windows can claim a keyboard collection even while its PnP identity is readable.
    wchar_t hardware[1024]{};
    DWORD type = 0;
    if (!SetupDiGetDeviceRegistryPropertyW(set, &info, SPDRP_HARDWAREID, &type,
            reinterpret_cast<PBYTE>(hardware), sizeof(hardware), nullptr) || type != REG_MULTI_SZ) return -1;
    for (size_t start = 0; start < std::size(hardware) && hardware[start];) {
        size_t end = start;
        while (end < std::size(hardware) && hardware[end]) ++end;
        const std::wstring id(hardware + start, end - start);
        if (id == L"HID_DEVICE_SYSTEM_MOUSE") return 0;
        if (id == L"HID_DEVICE_SYSTEM_KEYBOARD") return 2;
        if (id == L"HID_DEVICE_SYSTEM_GAME" || id == L"HID_DEVICE_UP:0001_U:0005" ||
                id == L"HID_DEVICE_UP:0001_U:0004") return gamepadSlot;
        start = end + 1;
    }
    return -1;
}

void FindUsbDevices(std::vector<Device>& devices) {
    struct Candidate {
        std::wstring id;
        std::wstring container;
        std::wstring name;
        std::wstring windowsName;
        int kind;
        int rank;
    };
    std::vector<Candidate> candidates;

    GUID hid{};
    HidD_GetHidGuid(&hid);
    ForInterfaces(hid, [&](HDEVINFO set, SP_DEVINFO_DATA& info, const std::wstring& path) {
        const auto lowerPath = Lower(path);
        if (lowerPath.compare(0, 12, L"\\\\?\\hid#vid_") != 0) return;
        const auto profile = std::find_if(std::begin(receiverProfiles), std::end(receiverProfiles),
            [&](const ReceiverProfile& item) { return MatchesReceiver(lowerPath, item); });
        if (profile != std::end(receiverProfiles)) {
            auto& device = Upsert(devices, profile->slotId);
            if (device.name.empty()) device.name = profile->name;
            if (profile->wireless && device.container.empty()) device.container = ContainerId(info.DevInst);
            device.kind = profile->kind;
            device.hidPaths.push_back(path);
            if (profile->wireless) return;
        }

        DEVINST usbInterface = 0;
        if (CM_Get_Parent(&usbInterface, info.DevInst, 0) != CR_SUCCESS) return;
        auto id = ConnectedPnpId(usbInterface);
        if (id.compare(0, 8, L"usb\\vid_") != 0) return;
        DEVINST physical = usbInterface;
        if (id.find(L"&mi_") != id.npos) {
            if (CM_Get_Parent(&physical, usbInterface, 0) != CR_SUCCESS) return;
            const auto parentId = ConnectedPnpId(physical);
            if (parentId.compare(0, 21, id, 0, 21) != 0 || parentId.size() < 22 || parentId[21] != L'\\')
                return;
            id = parentId;
        }

        const bool k75 = id.starts_with(L"usb\\vid_258a&pid_0149\\") ||
                         id.starts_with(L"usb\\vid_258a&pid_01aa\\");
        const auto handle = OpenReceiver(path, 0, 0);
        const int bootKind = UsbBootKind(usbInterface);
        const int hidKind = HidDeviceKind(set, info, handle.get());
        const int kind = hidKind < 0 ? bootKind : hidKind;
        if (kind < 0 || (k75 && kind != 2)) return;
        auto name = UsbBusProduct(physical);
        if (name.empty() && handle) {
            wchar_t product[256]{};
            if (HidD_GetProductString(handle.get(), product, sizeof(product))) name = product;
        }
        auto windowsName = name;
        if (k75) name = k75ModelName;
        if (IsReceiverDescription(name)) return;
        const int rank = (ClassifyName(name) == kind ? 4 : 0) + (bootKind == kind ? 2 : 0) +
                         (kind == 0 ? 1 : 0);

        const auto existing = std::find_if(candidates.begin(), candidates.end(), [&](const Candidate& item) {
            return item.id == id;
        });
        if (existing != candidates.end() && existing->rank >= rank) return;
        auto container = ContainerId(physical);
        if (container.empty()) container = ContainerId(info.DevInst);
        Candidate candidate{std::move(id), std::move(container), std::move(name), std::move(windowsName), kind, rank};
        if (existing != candidates.end()) *existing = std::move(candidate);
        else candidates.push_back(std::move(candidate));
    });

    for (const auto& candidate : candidates) {
        const auto profile = std::find_if(std::begin(receiverProfiles), std::end(receiverProfiles),
            [&](const ReceiverProfile& item) { return !item.wireless && HasUsbId(candidate.id, item); });
        const auto id = L"usb:" + candidate.id;
        auto& device = Upsert(devices, profile == std::end(receiverProfiles) ? id : profile->slotId);
        if (device.address != id) device.alternateIds.push_back(id);
        device.connection = Connection::usb;
        device.container = candidate.container;
        if (device.name.empty()) device.name = candidate.name;
        device.windowsName = candidate.windowsName;
        device.kind = profile == std::end(receiverProfiles) ? candidate.kind : profile->kind;
    }
}

struct ConnectedAudioCom {
    explicit ConnectedAudioCom(HRESULT result) : result(result) {}
    ~ConnectedAudioCom() { if (SUCCEEDED(result)) CoUninitialize(); }
    HRESULT result;
};

struct ReleaseConnectedAudio {
    void operator()(IUnknown* object) const { object->Release(); }
};

struct FreeConnectedAudioText {
    void operator()(wchar_t* text) const { CoTaskMemFree(text); }
};

struct ConnectedAudioValue {
    PROPVARIANT data{};
    ~ConnectedAudioValue() { PropVariantClear(&data); }
};

constexpr GUID connectedAudioEnumerator{0xbcde0395,0xe52f,0x467c,{0x8e,0x3d,0xc4,0x57,0x92,0x91,0x69,0x2e}};
constexpr PROPERTYKEY audioFormKey{{0x1da5d803,0xd492,0x4edd,{0x8c,0x23,0xe0,0xc0,0xff,0xee,0x7f,0x0e}},0};
constexpr PROPERTYKEY audioNameKey{{0xa45c254e,0xdf1c,0x4efd,{0x80,0x20,0x67,0xd1,0x46,0xa8,0x50,0xe0}},14};
constexpr PROPERTYKEY audioAdapterKey{{0x026e516e,0xb814,0x414b,{0x83,0xcd,0x85,0x6d,0x6f,0xef,0x48,0x22}},2};
constexpr PROPERTYKEY audioContainerKey{containerKey.fmtid,containerKey.pid};

std::wstring ConnectedAudioText(IPropertyStore& store, const PROPERTYKEY& key) {
    ConnectedAudioValue value;
    if (FAILED(store.GetValue(key, &value.data)) || value.data.vt != VT_LPWSTR || !value.data.pwszVal) return {};
    return value.data.pwszVal;
}

std::wstring ConnectedAudioContainer(IPropertyStore& store) {
    ConnectedAudioValue value;
    if (FAILED(store.GetValue(audioContainerKey, &value.data)) || value.data.vt != VT_CLSID ||
        !value.data.puuid || IsEqualGUID(*value.data.puuid, GUID{})) return {};
    wchar_t text[39]{};
    return StringFromGUID2(*value.data.puuid, text, std::size(text)) ? Lower(text) : L"";
}

Connection AudioEndpointConnection(const std::wstring& endpoint) {
    auto instanceId = L"SWD\\MMDEVAPI\\" + endpoint;
    DEVINST node = 0;
    if (CM_Locate_DevNodeW(&node, instanceId.data(), CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS)
        return Connection::other;
    for (size_t depth = 0; depth < 8 && CM_Get_Parent(&node, node, 0) == CR_SUCCESS; ++depth) {
        const auto id = ConnectedPnpId(node);
        if (id.compare(0, 8, L"bthenum\\") == 0 || id.compare(0, 6, L"bthle\\") == 0 ||
            id.compare(0, 12, L"bthledevice\\") == 0 || id.compare(0, 11, L"bthhfenum\\") == 0)
            return Connection::bluetooth;
        if (id.starts_with(L"usb\\vid_")) {
            for (const auto& profile : receiverProfiles)
                if (profile.kind == 1 && profile.wireless && HasUsbId(id, profile)) return Connection::receiver;
            return Connection::usb;
        }
    }
    return Connection::other;
}

void FindConnectedHeadphones(std::vector<Device>& devices) {
    const ConnectedAudioCom com(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    if (FAILED(com.result) && com.result != RPC_E_CHANGED_MODE) return;

    IMMDeviceEnumerator* rawEnumerator = nullptr;
    if (FAILED(CoCreateInstance(connectedAudioEnumerator, nullptr, CLSCTX_INPROC_SERVER,
            __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&rawEnumerator)))) return;
    const std::unique_ptr<IMMDeviceEnumerator, ReleaseConnectedAudio> enumerator(rawEnumerator);
    IMMDeviceCollection* rawEndpoints = nullptr;
    if (FAILED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &rawEndpoints))) return;
    const std::unique_ptr<IMMDeviceCollection, ReleaseConnectedAudio> endpoints(rawEndpoints);
    UINT count = 0;
    if (FAILED(endpoints->GetCount(&count))) return;

    for (UINT index = 0; index < count; ++index) {
        IMMDevice* rawDevice = nullptr;
        if (FAILED(endpoints->Item(index, &rawDevice))) continue;
        const std::unique_ptr<IMMDevice, ReleaseConnectedAudio> endpoint(rawDevice);
        IPropertyStore* rawStore = nullptr;
        if (FAILED(endpoint->OpenPropertyStore(STGM_READ, &rawStore))) continue;
        const std::unique_ptr<IPropertyStore, ReleaseConnectedAudio> store(rawStore);
        ConnectedAudioValue form;
        if (FAILED(store->GetValue(audioFormKey, &form.data)) || form.data.vt != VT_UI4 ||
            (form.data.uintVal != Headphones && form.data.uintVal != Headset)) continue;

        wchar_t* rawId = nullptr;
        if (FAILED(endpoint->GetId(&rawId))) continue;
        const std::unique_ptr<wchar_t, FreeConnectedAudioText> endpointId(rawId);
        const auto name = ConnectedAudioText(*store, audioNameKey);
        const auto adapter = ConnectedAudioText(*store, audioAdapterKey);
        const auto container = ConnectedAudioContainer(*store);
        const auto match = std::find_if(devices.begin(), devices.end(), [&](const Device& device) {
            return !container.empty() && device.container == container &&
                   (device.kind == 1 || (device.kind < 0 && !device.lePath.empty()));
        });
        if (match != devices.end()) {
            if (match->connected() && match->kind < 0) match->kind = 1;
            continue;
        }
        if (IsReceiverDescription(name) || IsReceiverDescription(adapter)) continue;
        const auto connection = AudioEndpointConnection(endpointId.get());
        if (connection == Connection::receiver || connection == Connection::bluetooth) continue;
        if (connection == Connection::other &&
            (Lower(name).find(L"g733") != std::wstring::npos || Lower(adapter).find(L"g733") != std::wstring::npos))
            continue;

        auto& device = Upsert(devices, L"audio:" + Lower(endpointId.get()));
        device.connection = connection;
        device.kind = 1;
        device.windowsName = !name.empty() ? name : adapter;
        device.name = device.windowsName;
        device.container = container;
    }
}

BluetoothReading ReadLowEnergyBattery(const Device& device, const std::wstring& servicePath) {
    if (!servicePath.empty()) {
        const auto serviceHandle = CreateFileW(servicePath.c_str(), GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (serviceHandle != INVALID_HANDLE_VALUE) {
            const std::unique_ptr<void, CloseFile> serviceFile(serviceHandle);
            const auto reading = ReadBatteryCharacteristic(serviceFile.get(), nullptr);
            if (reading.level != unknown || reading.invalidLevel != unknown || reading.charging) return reading;
        }
    }
    auto handle = CreateFileW(device.lePath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return {};
    std::unique_ptr<void, CloseFile> deviceFile(handle);
    USHORT count = 0;
    BluetoothGATTGetServices(handle, 0, nullptr, &count, BLUETOOTH_GATT_FLAG_NONE);
    if (!count) return {};
    std::vector<BTH_LE_GATT_SERVICE> services(count);
    if (FAILED(BluetoothGATTGetServices(handle, count, services.data(), &count, BLUETOOTH_GATT_FLAG_NONE)))
        return {};
    BluetoothReading reading;
    for (auto& service : services) {
        if (!IsUuid(service.ServiceUuid, 0x180f, batteryService)) continue;
        const auto result = ReadBatteryCharacteristic(deviceFile.get(), &service);
        if (result.level != unknown) return result;
        if (result.charging || (!reading.charging && result.invalidLevel != unknown)) reading = result;
    }
    return reading;
}

bool FitsSlot(const Device& device, size_t slot) {
    return device.kind == static_cast<int>(slot);
}

bool MatchesChoice(const Device& device, const std::wstring& choice) {
    return device.address == choice ||
           std::find(device.alternateIds.begin(), device.alternateIds.end(), choice) != device.alternateIds.end();
}

const std::wstring& ChoiceForSlot(const std::vector<Device>& devices, size_t slot,
                                  const std::wstring& savedChoice) {
    static const std::wstring automatic;
    if (savedChoice.empty()) return savedChoice;
    const auto selected = std::find_if(devices.begin(), devices.end(), [&](const Device& device) {
        return MatchesChoice(device, savedChoice);
    });
    if (selected != devices.end() && !FitsSlot(*selected, slot)) return automatic;
    // Recognize older cross-slot selections even when their USB receiver is unplugged.
    for (const auto& profile : receiverProfiles) {
        if (savedChoice == profile.slotId && profile.kind != static_cast<int>(slot)) return automatic;
    }
    return savedChoice;
}

size_t Select(const std::vector<Device>& devices, size_t slot, const std::wstring& savedChoice) {
    const auto& choice = ChoiceForSlot(devices, slot, savedChoice);
    size_t match = devices.size();
    for (size_t i = 0; i < devices.size(); ++i) {
        const auto& device = devices[i];
        if (!device.connected() || !FitsSlot(device, slot)) continue;
        if (!choice.empty()) {
            if (MatchesChoice(device, choice)) return i;
        } else {
            if (device.connection == Connection::receiver || !device.hidPaths.empty()) return i;
            if (match == devices.size() ||
                (device.connection == Connection::bluetooth && devices[match].connection != Connection::bluetooth))
                match = i;
        }
    }
    return match;
}

bool IsK75Model(const Device& device) {
    if (!FitsSlot(device, keyboardSlot)) return false;
    auto name = Lower(device.name);
    name.erase(std::remove_if(name.begin(), name.end(), [](wchar_t ch) { return ch == L' ' || ch == L'-'; }), name.end());
    return name.starts_with(L"kzzik75pro");
}

bool IsK75Wired(const Device& device) {
    return device.address.starts_with(L"usb:usb\\vid_258a&pid_0149\\") ||
           device.address.starts_with(L"usb:usb\\vid_258a&pid_01aa\\");
}

void MergeK75Connections(std::vector<Device>& devices) {
    // A direct USB connection proves the wired mode even if a receiver still reports its old link.
    auto keyboard = std::find_if(devices.begin(), devices.end(), [](const Device& device) {
        return device.connection == Connection::usb && IsK75Wired(device);
    });
    if (keyboard == devices.end()) keyboard = std::find_if(devices.begin(), devices.end(), [](const Device& device) {
        return device.connection == Connection::receiver && IsK75Model(device);
    });
    if (keyboard == devices.end()) keyboard = std::find_if(devices.begin(), devices.end(), [](const Device& device) {
        return device.connected() && IsK75Model(device);
    });
    if (keyboard == devices.end()) keyboard = std::find_if(devices.begin(), devices.end(), [](const Device& device) {
        return device.connected() && device.address == L"usb-keyboard";
    });
    if (keyboard == devices.end()) return;
    keyboard->alternateIds.push_back(L"usb-keyboard");
    for (auto& other : devices) {
        if (&other == &*keyboard || (other.address != L"usb-keyboard" && !IsK75Model(other))) continue;
        // Keep independently connected boards of the same model as distinct entries.
        if (other.connected() && other.address != L"usb-keyboard") continue;
        keyboard->alternateIds.push_back(other.address);
        other.connection = Connection::disconnected;
    }
    if (keyboard->address == L"usb-keyboard" && std::any_of(devices.begin(), devices.end(), IsK75Model))
        keyboard->name = k75ModelName;
}

std::wstring DeviceIdentity(const Device& device) {
    if (IsK75Wired(device) || (IsK75Model(device) &&
        std::find(device.alternateIds.begin(), device.alternateIds.end(), L"usb-keyboard") != device.alternateIds.end()))
        return L"usb-keyboard";
    if (!device.container.empty()) return L"container:" + device.container;
    return device.address;
}

void KeepKnownK75Receiver(Device& device, const LastSample& remembered, const std::vector<Device>& previous) {
    if (device.address != L"usb-keyboard" || device.connection != Connection::receiver ||
        device.battery != unknown || remembered.identity != device.address || IsK75Model(device)) return;
    const auto lastReceiver = std::find_if(previous.begin(), previous.end(), [&](const Device& item) {
        return item.address == device.address;
    });
    if (lastReceiver != previous.end() && !lastReceiver->container.empty() && !device.container.empty() &&
        lastReceiver->container != device.container) return;
    // A link-only response lacks the model signature. Keep the last confirmed name and cache identity.
    device.name = k75ModelName;
    if (!remembered.name.empty()) device.windowsName = remembered.name;
}

void Poll(Scan& scan) {
    FindClassic(scan.devices);
    FindLowEnergy(scan.devices);
    MarkBluetoothConnections(scan.devices);
    FindUsbDevices(scan.devices);
    for (auto& device : scan.devices) {
        if (device.name.empty()) device.name = device.address;
        if (device.kind == -1) device.kind = ClassifyName(device.name);
        if (!device.hidPaths.empty()) ReadReceiverBattery(device);
    }
    ClassifyBluetoothHid(scan.devices);
    FindConnectedHeadphones(scan.devices);
    MergeK75Connections(scan.devices);
    for (auto& device : scan.devices) {
        if (!device.connected()) continue;
        auto name = WindowsContainerName(device.container);
        if (!name.empty()) device.windowsName = std::move(name);
    }
    std::sort(scan.devices.begin(), scan.devices.end(), [](const Device& a, const Device& b) {
        return a.name == b.name ? a.address < b.address : a.name < b.name;
    });
    std::array<Device*, slotCount> selected{};
    for (size_t i = 0; i < selected.size(); ++i) {
        scan.selected[i] = Select(scan.devices, i, scan.choices[i]);
        if (scan.selected[i] < scan.devices.size()) selected[i] = &scan.devices[scan.selected[i]];
    }
    FindClassicBattery(selected);
    std::array<std::wstring, slotCount> servicePaths{};
    if (std::any_of(selected.begin(), selected.end(), [](const Device* device) {
        return device && !device->lePath.empty();
    })) {
        ForInterfaces(leServiceInterface, [&](HDEVINFO, SP_DEVINFO_DATA& node, const std::wstring& path) {
            if (Lower(path).find(L"0000180f-0000-1000-8000-00805f9b34fb") == std::wstring::npos) return;
            auto address = AddressOf(node.DevInst);
            if (address.empty()) address = AddressFromPath(path);
            for (size_t i = 0; i < selected.size(); ++i)
                if (selected[i] && selected[i]->address == address) servicePaths[i] = path;
        });
    }
    for (size_t i = 0; i < selected.size(); ++i) {
        auto device = selected[i];
        if (!device || device->lePath.empty()) continue;
        const auto reading = ReadLowEnergyBattery(*device, servicePaths[i]);
        if (reading.level != unknown) device->battery = reading.level;
        device->invalidBluetoothLevel = reading.invalidLevel;
        device->charging = reading.charging;
    }
}

struct TrayPalette {
    DWORD ink, track, charging, muted;
    unsigned trackOpacity;
    bool highContrast = false;
    bool operator==(const TrayPalette&) const = default;
};
constexpr TrayPalette darkTray{
    0xf1f6fc, 0x748598, 0xd7acff, 0xa5b8ca, 130};
constexpr TrayPalette lightTray{
    0x192d42, 0x49596e, 0x8540e8, 0x536d87, 185};
TrayPalette trayPalette = darkTray;

DWORD RgbValue(COLORREF color) {
    return (DWORD(GetRValue(color)) << 16) | (DWORD(GetGValue(color)) << 8) | GetBValue(color);
}

TrayPalette CurrentTrayPalette() {
    HIGHCONTRASTW contrast{};
    contrast.cbSize = sizeof(contrast);
    if (SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0) &&
        (contrast.dwFlags & HCF_HIGHCONTRASTON)) {
        const auto foreground = RgbValue(GetSysColor(COLOR_WINDOWTEXT));
        const auto selected = RgbValue(GetSysColor(COLOR_HOTLIGHT));
        return {foreground, foreground, selected, foreground, 180, true};
    }
    if (appTheme == AppTheme::light) return lightTray;
    if (appTheme == AppTheme::dark) return darkTray;
    constexpr wchar_t personalization[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize";
    DWORD light = 0, size = sizeof(light);
    RegGetValueW(HKEY_CURRENT_USER, personalization, L"SystemUsesLightTheme",
                 RRF_RT_REG_DWORD, nullptr, &light, &size);
    return light ? lightTray : darkTray;
}

float RoundedBox(float x, float y, float cx, float cy, float halfWidth, float halfHeight, float radius) {
    const float dx = std::abs(x - cx) - halfWidth + radius;
    const float dy = std::abs(y - cy) - halfHeight + radius;
    const float px = std::max(dx, 0.0f), py = std::max(dy, 0.0f);
    return std::sqrt(px * px + py * py) + std::min(std::max(dx, dy), 0.0f) - radius;
}

bool TrayDeviceShape(size_t index, float x, float y) {
    if (index == 0) {
        const bool outline = std::abs(RoundedBox(x, y, 8, 5.48f, 4.07f, 4.68f, 3.55f)) <= .83f;
        const bool divider = RoundedBox(x, y, 8, 2.06f, .42f, 1.27f, .31f) <= 0;
        const bool wheel = RoundedBox(x, y, 8, 4.03f, .89f, 1.22f, .69f) <= 0;
        return outline || divider || wheel;
    }
    if (index == 1) {
        constexpr float innerRadius = 4.38f, outerRadius = 6.27f;
        const float distanceX = std::abs(x - 8), distanceY = y - 6.50f;
        const float radiusSquared = distanceX * distanceX + distanceY * distanceY;
        const bool headband = distanceY <= 0 && radiusSquared >= innerRadius * innerRadius &&
                              radiusSquared <= outerRadius * outerRadius;
        const bool cup = RoundedBox(distanceX, y, 5.50f, 8.48f, 1.89f, 2.45f, 1.14f) <= 0;
        const bool inset = RoundedBox(distanceX, y, 5.50f, 8.68f, .58f, 1.23f, .46f) <= 0;
        return headband || (cup && !inset);
    }
    if (index == gamepadSlot) {
        const float gripX = std::abs(x - 8) - (y - 6.6f) * .46f;
        const bool body = RoundedBox(x, y, 8, 5.2f, 6.42f, 3.7f, 2.72f) <= 0 ||
                          RoundedBox(gripX, y, 4.27f, 8.07f, 2.18f, 2.88f, 1.84f) <= 0;
        if (!body) return false;
        const float padX = std::abs(x - 4.40f), padY = std::abs(y - 5.50f);
        const bool directionPad = (padX <= .65f && padY <= 1.58f) || (padX <= 1.58f && padY <= .65f);
        const bool buttons = RoundedBox(x, y, 11.44f, 4.33f, .98f, .98f, .98f) <= 0 ||
                             RoundedBox(x, y, 12.45f, 6.39f, .90f, .90f, .90f) <= 0;
        return !directionPad && !buttons;
    }
    if (RoundedBox(x, y, 8, 5.91f, 7.62f, 4.96f, 1.78f) > 0) return false;
    const float centerX = std::abs(x - 8);
    const float nearestKey = std::min(std::abs(centerX - 1.59f), std::abs(centerX - 4.75f));
    const bool keys = RoundedBox(nearestKey, y, 0, 4.58f, .94f, .96f, .25f) <= 0 ||
                      RoundedBox(x, y, 8, 7.94f, 4.11f, .92f, .36f) <= 0;
    return !keys;
}

// A shallow lower arc leaves room for a full-height device and one clear row.
constexpr float trayArcLeft = 1.48f, trayArcWidth = 13.04f, trayArcTop = 13.40f, trayArcBend = 3.76f;

float TrayArcY(float position) {
    return trayArcTop + trayArcBend * position * (1 - position);
}

float TrayArcPosition(float x, float y) {
    const float position = std::clamp((x - trayArcLeft) / trayArcWidth, 0.0f, 1.0f);
    const float dx = trayArcLeft + trayArcWidth * position - x;
    const float dy = TrayArcY(position) - y;
    const float slope = trayArcBend * (1 - 2 * position);
    const float derivative = trayArcWidth * trayArcWidth + slope * slope - 2 * trayArcBend * dy;
    return std::clamp(position - (dx * trayArcWidth + dy * slope) / derivative, 0.0f, 1.0f);
}

float TrayArcDistanceSquared(float x, float y, float position) {
    const float dx = x - (trayArcLeft + trayArcWidth * position);
    const float dy = y - TrayArcY(position);
    return dx * dx + dy * dy;
}

using TrayNumberRows = std::array<unsigned, 7>;

TrayNumberRows TrayNumberMask(int level) {
    constexpr std::array<TrayNumberRows, 10> numerals{{
        {0b111, 0b101, 0b101, 0b101, 0b101, 0b101, 0b111},
        {0b01, 0b11, 0b01, 0b01, 0b01, 0b01, 0b11},
        {0b111, 0b001, 0b001, 0b111, 0b100, 0b100, 0b111},
        {0b111, 0b001, 0b001, 0b111, 0b001, 0b001, 0b111},
        {0b101, 0b101, 0b101, 0b111, 0b001, 0b001, 0b001},
        {0b111, 0b100, 0b100, 0b111, 0b001, 0b001, 0b111},
        {0b111, 0b100, 0b100, 0b111, 0b101, 0b101, 0b111},
        {0b111, 0b001, 0b001, 0b010, 0b010, 0b100, 0b100},
        {0b111, 0b101, 0b101, 0b111, 0b101, 0b101, 0b111},
        {0b111, 0b101, 0b101, 0b111, 0b001, 0b001, 0b111},
    }};
    constexpr int areaWidth = 16;
    TrayNumberRows rows{};
    if (level == unknown) {
        rows[3] = 0b1111 << (16 - (areaWidth + 4) / 2);
        return rows;
    }

    const std::array digits{level / 100, level / 10 % 10, level % 10};
    const int first = level >= 100 ? 0 : level >= 10 ? 1 : 2;
    int digitWidth = 0;
    for (int i = first; i < 3; ++i) {
        const int width = digits[i] == 1 ? 3 : 4;
        for (size_t row = 0; row < rows.size(); ++row) {
            const unsigned pattern = numerals[digits[i]][row];
            // Duplicate the middle column, or the leading column of the narrow numeral 1.
            const unsigned widened = ((pattern & ~1u) << 1) | (pattern & 0b11u);
            rows[row] = (rows[row] << (width + 1)) | widened;
        }
        digitWidth += width + 1;
    }
    --digitWidth;
    const int left = (areaWidth - digitWidth) / 2;
    for (auto& row : rows) row <<= 16 - left - digitWidth;
    return rows;
}

constexpr DWORD BatteryGapColor(const TrayPalette& palette) {
    return palette.highContrast ? palette.ink : 0x8e9daa;
}

constexpr DWORD ChargingColor(const TrayPalette& palette, const BatteryAppearance& appearance) {
    return palette.highContrast || appearance.chargingColor == themeChargingColor
        ? palette.charging : appearance.chargingColor;
}

constexpr DWORD BatteryFillColor(const TrayPalette& palette, const BatteryImage& status,
                                 const BatteryAppearance& appearance) {
    if (status.stale || status.level == unknown) return palette.muted;
    if (status.charging) return ChargingColor(palette, appearance);
    const auto band = BatteryBand(appearance, status.level);
    if (band == appearance.ranges.size()) return BatteryGapColor(palette);
    return palette.highContrast ? palette.charging : appearance.colors[band];
}

constexpr bool IsFactoryBatteryColor(size_t band, DWORD color) {
    constexpr BatteryAppearance defaults{};
    constexpr std::array<DWORD, 3> previousDefaults{0xff7d82, 0xffd85a, 0x65f3b2};
    return color == defaults.colors[band] || color == previousDefaults[band];
}

constexpr DWORD LightBatteryTone(size_t band, DWORD color) {
    const std::array<DWORD, 3> lightTones{0xb7314a, color, 0x167144};
    return IsFactoryBatteryColor(band, color) ? lightTones[band] : color;
}

constexpr DWORD TrayMeterColor(const TrayPalette& palette, const BatteryImage& status,
                               const BatteryAppearance& appearance) {
    if (status.charging) return ChargingColor(palette, appearance);
    if (palette.highContrast) return palette.ink;
    if (palette != lightTray || status.stale || status.level == unknown)
        return BatteryFillColor(palette, status, appearance);
    const auto band = BatteryBand(appearance, status.level);
    if (band == appearance.ranges.size()) return palette.muted;
    return LightBatteryTone(band, appearance.colors[band]);
}

HICON MakeTrayIcon(size_t index, const Slot& slot, const BatteryImage& status) {
    const int width = GetSystemMetrics(SM_CXSMICON);
    const int height = GetSystemMetrics(SM_CYSMICON);
    if (width <= 0 || height <= 0) return nullptr;
    const size_t stride = ((width + 15) / 16) * 2;
    std::vector<BYTE> maskBits(stride * height);

    BITMAPINFO bitmap{};
    bitmap.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmap.bmiHeader.biWidth = width;
    bitmap.bmiHeader.biHeight = -height;
    bitmap.bmiHeader.biPlanes = 1;
    bitmap.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    const HBITMAP color = CreateDIBSection(nullptr, &bitmap, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!color) return nullptr;
    auto pixels = static_cast<DWORD*>(bits);

    const auto& palette = trayPalette;
    const bool known = status.level != unknown;
    const bool light = palette == lightTray;
    const int level = known ? std::clamp(status.level, 0, 100) : unknown;
    const DWORD glyph = known && !status.stale ? palette.ink : palette.muted;
    const DWORD fill = TrayMeterColor(palette, status, slot.appearance);
    const bool emptyCharging = status.charging && level <= 0;
    const bool showUnavailableMark = !known && !status.charging && !slot.numbers;
    const float fillStart = showUnavailableMark ? .36f : 0;
    const float fillEnd = showUnavailableMark ? .64f : std::max(0, level) / 100.0f;
    const auto numberRows = slot.numbers ? TrayNumberMask(level) : TrayNumberRows{};
    constexpr std::array<int, 10> numeralRow{0, 0, 1, 2, 3, 3, 4, 5, 6, 6};

    constexpr int steps = 6;
    constexpr unsigned sampleCount = steps * steps;
    constexpr unsigned colorSamples = 255 * sampleCount;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            unsigned alphaTotal = 0, red = 0, green = 0, blue = 0;
            for (int sy = 0; sy < steps; ++sy) {
                const float v = (y + (sy + .5f) / steps) * 16 / height;
                if (v >= 11 && v < 12) continue;
                for (int sx = 0; sx < steps; ++sx) {
                    const float u = (x + (sx + .5f) / steps) * 16 / width;
                    DWORD sample;
                    unsigned opacity = 255;
                    if (v < 11) {
                        const bool mark = slot.numbers
                            ? v >= 1 && (numberRows[numeralRow[static_cast<int>(v) - 1]] & (1u << (15 - static_cast<int>(u))))
                            : TrayDeviceShape(index, u, v);
                        if (!mark) continue;
                        sample = glyph;
                    } else {
                        constexpr float keylineRadius = 1.46f, fillRadius = 1.17f, trackRadius = 1.02f;
                        if (u < trayArcLeft - keylineRadius || u > trayArcLeft + trayArcWidth + keylineRadius ||
                            v > TrayArcY(.5f) + keylineRadius) continue;
                        const float position = TrayArcPosition(u, v);
                        const float distance = TrayArcDistanceSquared(u, v, position);
                        if (distance > keylineRadius * keylineRadius) continue;
                        if (emptyCharging) {
                            // The hollow outline marks charging without implying a measured percentage.
                            constexpr float hollowRadius = .72f;
                            if (distance < hollowRadius * hollowRadius) continue;
                            sample = fill;
                        } else {
                            const float fillDistance = fillEnd > fillStart
                                ? TrayArcDistanceSquared(u, v, std::clamp(position, fillStart, fillEnd)) : 3;
                            if (fillDistance <= fillRadius * fillRadius) {
                                sample = fill;
                            } else if (light && fillDistance <= keylineRadius * keylineRadius) {
                                sample = 0x3d485a;
                            } else {
                                if (distance > trackRadius * trackRadius) continue;
                                sample = palette.track;
                                opacity = palette.trackOpacity;
                            }
                        }
                    }
                    alphaTotal += opacity;
                    red += ((sample >> 16) & 255) * opacity;
                    green += ((sample >> 8) & 255) * opacity;
                    blue += (sample & 255) * opacity;
                }
            }
            const DWORD alpha = (alphaTotal + sampleCount / 2) / sampleCount;
            pixels[y * width + x] = (alpha << 24) | (((red + colorSamples / 2) / colorSamples) << 16) |
                (((green + colorSamples / 2) / colorSamples) << 8) | ((blue + colorSamples / 2) / colorSamples);
            if (!alpha) maskBits[y * stride + x / 8] |= static_cast<BYTE>(0x80 >> (x % 8));
        }
    }
    const HBITMAP mask = CreateBitmap(width, height, 1, 1, maskBits.data());
    ICONINFO info{TRUE, 0, 0, mask, color};
    const HICON icon = mask ? CreateIconIndirect(&info) : nullptr;
    if (mask) DeleteObject(mask);
    DeleteObject(color);
    return icon;
}

bool Publish(HWND window, size_t index, DWORD operation) {
    const auto& slot = slots[index];
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = window;
    data.uID = static_cast<UINT>(index + 1);
    data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    data.uCallbackMessage = trayMessage;
    data.hIcon = slot.icon;
    wcsncpy_s(data.szTip, slot.tip.c_str(), _TRUNCATE);
    return Shell_NotifyIconW(operation, &data) != FALSE;
}

void ShowStatus(HWND window, size_t i, const BatteryImage& status, const std::wstring& detail, bool add = false) {
    auto& slot = slots[i];
    const auto tip = std::wstring(slot.label.get()) + L" — " + detail;
    if (!slot.inTray) {
        if (slot.registered) Publish(window, i, NIM_DELETE);
        slot.registered = false;
        if (slot.icon) DestroyIcon(slot.icon);
        slot.icon = nullptr;
        slot.shown = status;
        slot.tip = tip;
        return;
    }
    const bool changed = slot.shown != status || !slot.icon;
    if (changed) {
        HICON replacement = MakeTrayIcon(i, slot, status);
        if (replacement) {
            if (slot.icon) DestroyIcon(slot.icon);
            slot.icon = replacement;
        }
    }
    slot.shown = status;
    if (add) slot.registered = false;
    if (!slot.registered || changed || slot.tip != tip) {
        slot.tip = tip;
        slot.registered = slot.icon && Publish(window, i, slot.registered ? NIM_MODIFY : NIM_ADD);
    }
}

std::wstring TrayDeviceDetail(const Device& device, const BatteryImage& status) {
    std::wstring charge;
    if (status.level == unknown) {
        if (status.charging) charge = Text(L"Charging · percentage unavailable", L"заряжается · процент недоступен");
        else if (device.receiverUnlinked)
            charge = Text(L"Headset disconnected from receiver", L"гарнитура не подключена к приёмнику");
        else if (device.invalidBluetoothLevel != unknown)
            charge = Text(L"Bluetooth returned ", L"Bluetooth вернул ") + std::to_wstring(device.invalidBluetoothLevel) +
                     Text(L" outside 0–100", L" вне диапазона 0–100");
        else charge = Text(L"Battery level unavailable", L"заряд недоступен");
    } else {
        charge = std::to_wstring(status.level) + L"%";
        if (status.stale && device.receiverUnlinked)
            charge += Text(L" (last reading, headset disconnected)", L" (последний замер, гарнитура отключена)");
        else if (status.stale && status.charging)
            charge += Text(L" (last reading, charging)", L" (последний замер, заряжается)");
        else if (status.stale) charge += Text(L" (last reading)", L" (последний замер)");
        else if (status.charging) charge += Text(L" (charging)", L" (заряжается)");
    }
    auto detail = charge + L" · " + DisplayName(device);
    const std::wstring_view connection = ConnectionLabel(device.connection);
    if (!connection.empty()) detail.append(L" · ").append(connection);
    return detail;
}

void RepaintTrayIcon(HWND window, size_t index) {
    auto& slot = slots[index];
    if (slot.tip.empty()) return;
    const auto prefix = std::wstring(slot.label.get()) + L" — ";
    const auto detail = slot.tip.substr(prefix.size());
    if (slot.icon) DestroyIcon(slot.icon);
    slot.icon = nullptr;
    ShowStatus(window, index, slot.shown, detail);
}

void UpdateTrayPalette(HWND window) {
    const auto current = CurrentTrayPalette();
    if (current == trayPalette) return;
    trayPalette = current;
    for (size_t i = 0; i < slots.size(); ++i) {
        if (slots[i].inTray) RepaintTrayIcon(window, i);
    }
}

struct ScanMailbox {
    SRWLOCK lock = SRWLOCK_INIT;
    HWND owner = nullptr;
    Scan* ready = nullptr;
};
ScanMailbox scanMailbox;

std::unique_ptr<Scan> TakeScanResult(bool closing = false) {
    AcquireSRWLockExclusive(&scanMailbox.lock);
    if (closing) scanMailbox.owner = nullptr;
    std::unique_ptr<Scan> result(std::exchange(scanMailbox.ready, nullptr));
    ReleaseSRWLockExclusive(&scanMailbox.lock);
    return result;
}

DWORD WINAPI ScanWorker(void* state) {
    std::unique_ptr<Scan> scan(static_cast<Scan*>(state));
    Poll(*scan);
    AcquireSRWLockExclusive(&scanMailbox.lock);
    if (scanMailbox.owner == scan->window && !scanMailbox.ready) {
        // The window message is only a wakeup. Result ownership stays in the process.
        const auto window = scan->window;
        scanMailbox.ready = scan.release();
        PostMessageW(window, scanFinished, 0, 0);
    }
    ReleaseSRWLockExclusive(&scanMailbox.lock);
    return 0;
}

void Refresh(HWND window) {
    if (scanning) { rescan = true; return; }
    auto scan = std::make_unique<Scan>();
    scan->window = window;
    for (size_t i = 0; i < slots.size(); ++i) scan->choices[i] = slots[i].choice;
    if (QueueUserWorkItem(ScanWorker, scan.get(), WT_EXECUTELONGFUNCTION)) {
        scan.release();
        scanning = true;
    }
}

constexpr wchar_t soundScheme[] = L"AppEvents\\Schemes\\Apps\\.Default\\";
constexpr const wchar_t* soundEvents[] = {
    L"Notification.Default", L"Notification.IM", L"Notification.Mail", L"Notification.Reminder"
};
constexpr LocalizedText soundLabels[] = {
    {L"Windows notification", L"Уведомление Windows"},
    {L"Windows message", L"Сообщение Windows"},
    {L"Windows mail", L"Почта Windows"},
    {L"Windows reminder", L"Напоминание Windows"},
    {L"Custom WAV file", L"Свой WAV-файл"}
};

std::wstring ReadString(const std::wstring& subkey, const wchar_t* value) {
    constexpr DWORD flags = RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ;
    DWORD bytes = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, subkey.c_str(), value, flags, nullptr, nullptr, &bytes)
        != ERROR_SUCCESS || bytes < sizeof(wchar_t)) return {};
    std::wstring result(bytes / sizeof(wchar_t), L'\0');
    if (RegGetValueW(HKEY_CURRENT_USER, subkey.c_str(), value, flags, nullptr, result.data(), &bytes)
        != ERROR_SUCCESS) return {};
    result.resize(wcslen(result.c_str()));
    return result;
}

DWORD ReadNumber(const std::wstring& subkey, const wchar_t* value, DWORD fallback) {
    DWORD number = 0, bytes = sizeof(number);
    if (RegGetValueW(HKEY_CURRENT_USER, subkey.c_str(), value, RRF_RT_REG_DWORD, nullptr, &number, &bytes)
        != ERROR_SUCCESS) return fallback;
    return number;
}

bool WriteString(const std::wstring& subkey, const wchar_t* value, const std::wstring& text) {
    return RegSetKeyValueW(HKEY_CURRENT_USER, subkey.c_str(), value, REG_SZ, text.c_str(),
                          static_cast<DWORD>((text.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
}

bool WriteNumber(const std::wstring& subkey, const wchar_t* value, DWORD number) {
    return RegSetKeyValueW(HKEY_CURRENT_USER, subkey.c_str(), value, REG_DWORD, &number, sizeof(number))
        == ERROR_SUCCESS;
}

const std::wstring deviceNamesKey = std::wstring(settingsKey) + L"\\DeviceNames";
constexpr size_t maxAliasLength = 80;

bool SaveDeviceAlias(const std::wstring& id, const std::wstring& alias) {
    if (id.empty() || alias.size() > maxAliasLength) return false;
    if (!alias.empty()) return WriteString(deviceNamesKey, id.c_str(), alias);
    const auto result = RegDeleteKeyValueW(HKEY_CURRENT_USER, deviceNamesKey.c_str(), id.c_str());
    return result == ERROR_SUCCESS || result == ERROR_FILE_NOT_FOUND || result == ERROR_PATH_NOT_FOUND;
}

std::wstring SlotKey(const Slot& slot) {
    return std::wstring(settingsKey) + L"\\" + slot.key;
}

constexpr wchar_t lastSampleValue[] = L"LastSampleV1";
constexpr size_t maxSampleText = 4096;
using SampleHeader = std::array<DWORD, 4>;

bool PersistLastSample(const Slot& slot, const LastSample& sample) {
    const auto text = slot.choice + sample.identity + sample.name;
    if (text.size() > maxSampleText) return false;
    const SampleHeader header{static_cast<DWORD>(sample.level + 1), static_cast<DWORD>(sample.connection),
                              static_cast<DWORD>(slot.choice.size()), static_cast<DWORD>(sample.identity.size())};
    std::vector<BYTE> data(sizeof(header) + text.size() * sizeof(wchar_t));
    std::memcpy(data.data(), header.data(), sizeof(header));
    std::memcpy(data.data() + sizeof(header), text.data(), text.size() * sizeof(wchar_t));
    return RegSetKeyValueW(HKEY_CURRENT_USER, SlotKey(slot).c_str(), lastSampleValue, REG_BINARY,
        data.data(), static_cast<DWORD>(data.size())) == ERROR_SUCCESS;
}

LastSample ReadLastSample(const Slot& slot) {
    const auto key = SlotKey(slot);
    DWORD bytes = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, key.c_str(), lastSampleValue, RRF_RT_REG_BINARY,
            nullptr, nullptr, &bytes) != ERROR_SUCCESS || bytes < sizeof(SampleHeader) ||
        bytes > sizeof(SampleHeader) + maxSampleText * sizeof(wchar_t) ||
        (bytes - sizeof(SampleHeader)) % sizeof(wchar_t) != 0) return {};
    std::vector<BYTE> data(bytes);
    if (RegGetValueW(HKEY_CURRENT_USER, key.c_str(), lastSampleValue, RRF_RT_REG_BINARY,
            nullptr, data.data(), &bytes) != ERROR_SUCCESS || bytes != data.size()) return {};
    SampleHeader header{};
    std::memcpy(header.data(), data.data(), sizeof(header));
    const auto& [level, route, choiceLength, idLength] = header;
    const auto textSize = (bytes - sizeof(header)) / sizeof(wchar_t);
    if (level > 101 || route < static_cast<DWORD>(Connection::other) ||
        route > static_cast<DWORD>(Connection::bluetooth) || choiceLength > textSize ||
        idLength == 0 || idLength > textSize - choiceLength) return {};
    std::wstring text(textSize, L'\0');
    std::memcpy(text.data(), data.data() + sizeof(header), textSize * sizeof(wchar_t));
    if (text.find(L'\0') != std::wstring::npos || text.compare(0, choiceLength, slot.choice) != 0) return {};
    return {text.substr(choiceLength, idLength), text.substr(choiceLength + idLength),
            static_cast<Connection>(route), static_cast<int>(level) - 1};
}

void ForgetLastSample(Slot& slot) {
    slot.lastSample = {};
    RegDeleteKeyValueW(HKEY_CURRENT_USER, SlotKey(slot).c_str(), lastSampleValue);
}

std::wstring RememberedDefaultName(const Slot& slot) {
    return slot.lastSample.name.empty() ? slot.label.get() : slot.lastSample.name;
}

std::wstring RememberedName(const Slot& slot) {
    auto alias = ReadString(deviceNamesKey, slot.lastSample.identity.c_str());
    return alias.empty() ? RememberedDefaultName(slot) : alias;
}

BatteryImage BatteryForDisplay(Slot& slot, const Device& device) {
    LastSample sample{DeviceIdentity(device), DeviceModelName(device), device.connection, device.battery};
    if (sample.level == unknown && sample.identity == slot.lastSample.identity)
        sample.level = slot.lastSample.level;
    const bool stale = device.battery == unknown && sample.level != unknown;
    if (sample != slot.lastSample) {
        PersistLastSample(slot, sample);
        slot.lastSample = std::move(sample);
    }
    return {slot.lastSample.level, device.charging, stale};
}

void ShowRememberedStatus(HWND window, size_t index) {
    const auto& slot = slots[index];
    const auto& sample = slot.lastSample;
    if (!sample.available()) {
        ShowStatus(window, index, {}, slot.choice.empty()
            ? Text(L"No connected device", L"нет подключённого устройства")
            : Text(L"Selected device disconnected", L"выбранное устройство отключено"));
        return;
    }
    const auto detail = (sample.level == unknown ? std::wstring(Text(L"No response", L"нет ответа"))
        : std::to_wstring(sample.level) + Text(L"% (last reading)", L"% (последний замер)")) + L" · " + RememberedName(slot);
    ShowStatus(window, index, {sample.level, false, true}, detail);
}

AlertSettings ReadAlert(DWORD config, std::wstring file) {
    return {(config & 0x100) != 0, static_cast<int>(std::min(config & 0xff, DWORD(100))),
            static_cast<Sound>(std::min(config >> 16, DWORD(static_cast<int>(Sound::custom)))), std::move(file)};
}

// Packed config, three inclusive from/to pairs, three RGB colors, and an optional charging RGB.
using PreferenceHeader = std::array<DWORD, 11>;
using RangePreferenceHeader = std::array<DWORD, 10>;
using LegacyPreferenceHeader = std::array<DWORD, 6>;
constexpr wchar_t preferencesValue[] = L"PreferencesV12";
constexpr wchar_t rangePreferencesValue[] = L"PreferencesV11";
constexpr wchar_t legacyPreferencesValue[] = L"PreferencesV7";
constexpr size_t maxSoundPathChars = 32767;

template <size_t Size>
bool ReadPreferenceData(const std::wstring& key, const wchar_t* value, std::array<DWORD, Size>& header,
                        std::wstring& file) {
    DWORD bytes = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, key.c_str(), value, RRF_RT_REG_BINARY,
                     nullptr, nullptr, &bytes) != ERROR_SUCCESS || bytes < sizeof(header) ||
        bytes > sizeof(header) + maxSoundPathChars * sizeof(wchar_t) ||
        (bytes - sizeof(header)) % sizeof(wchar_t) != 0) return false;

    std::vector<BYTE> data(bytes);
    if (RegGetValueW(HKEY_CURRENT_USER, key.c_str(), value, RRF_RT_REG_BINARY,
                     nullptr, data.data(), &bytes) != ERROR_SUCCESS || bytes != data.size()) return false;
    std::memcpy(header.data(), data.data(), sizeof(header));
    std::wstring path((bytes - sizeof(header)) / sizeof(wchar_t), L'\0');
    std::memcpy(path.data(), data.data() + sizeof(header), path.size() * sizeof(wchar_t));
    if (path.find(L'\0') != std::wstring::npos) return false;
    file = std::move(path);
    return true;
}

bool ValidBatteryAppearance(const BatteryAppearance& appearance) {
    return appearance.chargingColor <= themeChargingColor && ValidBatteryRanges(appearance.ranges) &&
        std::all_of(appearance.colors.begin(), appearance.colors.end(), [](DWORD color) { return color <= 0xffffff; });
}

template <size_t Size>
bool DecodeBatteryAppearance(const std::array<DWORD, Size>& header, BatteryAppearance& appearance) {
    static_assert(Size == 10 || Size == 11);
    if constexpr (Size == 11) appearance.chargingColor = header[10];
    else appearance.chargingColor = themeChargingColor;
    for (size_t i = 0; i < appearance.ranges.size(); ++i) {
        const auto from = header[1 + 2 * i], to = header[2 + 2 * i];
        if (from > 100 || to > 100) return false;
        appearance.ranges[i] = {static_cast<int>(from), static_cast<int>(to)};
        appearance.colors[i] = header[7 + i];
    }
    return ValidBatteryAppearance(appearance);
}

void LoadPreferences(Slot& slot, const std::wstring& key) {
    std::wstring file;
    PreferenceHeader header{};
    RangePreferenceHeader ranges{};
    BatteryAppearance appearance;
    DWORD config = 0;
    if (ReadPreferenceData(key, preferencesValue, header, file) && DecodeBatteryAppearance(header, appearance)) {
        config = header[0];
    } else if (ReadPreferenceData(key, rangePreferencesValue, ranges, file) && DecodeBatteryAppearance(ranges, appearance)) {
        config = ranges[0];
    } else {
        LegacyPreferenceHeader legacy{};
        if (!ReadPreferenceData(key, legacyPreferencesValue, legacy, file) ||
            legacy[1] >= legacy[2] || legacy[2] >= 100) return;
        appearance = {};
        appearance.ranges = ContiguousBatteryRanges(static_cast<int>(legacy[1]), static_cast<int>(legacy[2]));
        std::copy_n(legacy.begin() + 3, appearance.colors.size(), appearance.colors.begin());
        if (!ValidBatteryAppearance(appearance)) return;
        config = legacy[0];
    }
    slot.alert = ReadAlert(config, std::move(file));
    slot.inTray = (config & 0x200) == 0;
    slot.appearance = appearance;
}

void LoadSettings() {
    const auto theme = ReadNumber(settingsKey, L"Theme", 0);
    appTheme = theme <= static_cast<DWORD>(AppTheme::dark) ? static_cast<AppTheme>(theme) : AppTheme::system;
    const auto language = ReadNumber(settingsKey, L"Language", static_cast<DWORD>(AppLanguage::English));
    g_language = language == static_cast<DWORD>(AppLanguage::Russian) ? AppLanguage::Russian : AppLanguage::English;
    for (auto& slot : slots) {
        slot.choice = ReadString(settingsKey, slot.key);
        const auto key = SlotKey(slot);
        slot.alert = ReadAlert(ReadNumber(key, L"Alert", 20 | 0x100), ReadString(key, L"SoundFile"));
        slot.appearance = {};
        slot.inTray = true;
        const int lowMax = std::min(slot.alert.threshold, 98);
        slot.appearance.ranges = ContiguousBatteryRanges(lowMax, std::max(74, lowMax + 1));
        LoadPreferences(slot, key);
        slot.numbers = ReadNumber(key, L"ShowNumbers", 0) != 0;
    }
}

bool SavePreferences(const Slot& slot, const AlertSettings& alert, const BatteryAppearance& appearance,
                     bool inTray) {
    if (!ValidBatteryAppearance(appearance) || alert.file.size() > maxSoundPathChars)
        return false;
    const auto key = SlotKey(slot);
    const DWORD config = DWORD(alert.threshold) | (alert.enabled ? 0x100 : 0) | (inTray ? 0 : 0x200)
        | (DWORD(alert.sound) << 16);
    PreferenceHeader header{config};
    for (size_t i = 0; i < appearance.ranges.size(); ++i) {
        header[1 + 2 * i] = DWORD(appearance.ranges[i].from);
        header[2 + 2 * i] = DWORD(appearance.ranges[i].to);
        header[7 + i] = appearance.colors[i];
    }
    header[10] = appearance.chargingColor;
    std::vector<BYTE> data(sizeof(header) + alert.file.size() * sizeof(wchar_t));
    std::memcpy(data.data(), header.data(), sizeof(header));
    std::memcpy(data.data() + sizeof(header), alert.file.data(), alert.file.size() * sizeof(wchar_t));
    return RegSetKeyValueW(HKEY_CURRENT_USER, key.c_str(), preferencesValue, REG_BINARY, data.data(),
                           static_cast<DWORD>(data.size())) == ERROR_SUCCESS;
}

std::wstring SoundPath(const AlertSettings& alert) {
    if (alert.sound == Sound::custom) return alert.file;
    return ReadString(std::wstring(soundScheme) + soundEvents[static_cast<size_t>(alert.sound)] + L"\\.Current",
                      nullptr);
}

struct SoundRequest {
    HWND window;
    std::wstring path;
};
std::deque<std::wstring> pendingSounds;
bool playingSound = false;

DWORD WINAPI PlayQueuedSound(void* context) {
    std::unique_ptr<SoundRequest> request(static_cast<SoundRequest*>(context));
    PlaySoundW(request->path.c_str(), nullptr, SND_FILENAME | SND_SYNC | SND_NODEFAULT);
    PostMessageW(request->window, soundFinished, 0, 0);
    return 0;
}

void StartNextSound(HWND window) {
    if (playingSound || pendingSounds.empty()) return;
    auto request = std::make_unique<SoundRequest>();
    request->window = window;
    request->path = std::move(pendingSounds.front());
    pendingSounds.pop_front();
    if (QueueUserWorkItem(PlayQueuedSound, request.get(), WT_EXECUTELONGFUNCTION)) {
        request.release();
        playingSound = true;
    }
}

void QueueSound(HWND window, std::wstring path) {
    if (path.empty()) return;
    pendingSounds.push_back(std::move(path));
    StartNextSound(window);
}

bool ShouldAlert(Slot& slot, const Device& device) {
    if (device.battery == unknown) return false;
    if (slot.alertDevice != device.address) {
        slot.alertDevice = device.address;
        slot.alreadyAlerted = false;
    }
    if (device.battery > slot.alert.threshold) slot.alreadyAlerted = false;
    if (!slot.alert.enabled || device.charging || slot.alreadyAlerted ||
        device.battery > slot.alert.threshold) return false;
    slot.alreadyAlerted = true;
    return true;
}

void ShowLowCharge(HWND window, size_t index, const Device& device) {
    NOTIFYICONDATAW notification{};
    notification.cbSize = sizeof(notification);
    notification.hWnd = window;
    notification.uID = static_cast<UINT>(index + 1);
    notification.uFlags = NIF_INFO;
    notification.dwInfoFlags = NIIF_WARNING | NIIF_NOSOUND;
    wcscpy_s(notification.szInfoTitle, Text(L"Low battery", L"Низкий заряд"));
    const auto body = DisplayName(device) + L" — " + std::to_wstring(device.battery) + L"%";
    wcsncpy_s(notification.szInfo, body.c_str(), _TRUNCATE);
    if (slots[index].registered) Shell_NotifyIconW(NIM_MODIFY, &notification);
    else {
        const auto visible = std::find_if(slots.begin(), slots.end(), [](const Slot& slot) {
            return slot.registered;
        });
        if (visible != slots.end()) {
            notification.uID = static_cast<UINT>(visible - slots.begin() + 1);
            Shell_NotifyIconW(NIM_MODIFY, &notification);
        }
    }
    QueueSound(window, SoundPath(slots[index].alert));
}

enum : int {
    enableAlert = 501, thresholdEdit, soundSelect, wavName, browseWav, previewSound,
    lowFrom, lowTo, mediumFrom, mediumTo, highFrom, highTo, resetAppearance, chargePreview, deviceAliasEdit,
    lowColor, mediumColor, highColor, chargingColorButton, showInTray, settingsPreview, settingsDevice, settingsAdvanced,
    settingsError, settingsMuted
};

constexpr std::array rangeFromEdits{lowFrom, mediumFrom, highFrom};
constexpr std::array rangeToEdits{lowTo, mediumTo, highTo};
constexpr int settingsWidth = 490, settingsHeight = 578;
constexpr int settingsSwitchBase = 1000;
constexpr RECT settingsRangeCard{20, 148, 470, 299};
constexpr RECT settingsAliasBox{232, 434, 470, 465};
constexpr int settingsRangeRows = 182, settingsRangeStep = 37;
constexpr RECT SettingsRangeBox(size_t row, bool to) {
    const int x = to ? 380 : 294;
    const int y = settingsRangeRows + static_cast<int>(row) * settingsRangeStep;
    return {x, y, x + 76, y + 31};
}

struct SettingsColors {
    COLORREF page, panel, input, border, text, secondary, accent, onAccent;
    COLORREF lineQuiet, rail, cyan, cyanTile, neutral;
};

struct SettingsDialog {
    HWND tray;
    size_t slot;
    AlertSettings alert;
    BatteryAppearance appearance;
    std::wstring deviceName;
    BatteryImage battery;
    std::wstring deviceId;
    std::wstring alias;
    bool inTray = true;
    SettingsColors colors{};
    DWORD defaultChargingColor = lightTray.charging;
    std::array<COLORREF, 16> customColors{};
    HBRUSH pageBrush = nullptr, panelBrush = nullptr, inputBrush = nullptr;
    HFONT titleFont = nullptr, sectionFont = nullptr, bodyFont = nullptr, smallFont = nullptr, tinyFont = nullptr;
    bool advanced = false, resetColors = false, saved = false;

    ~SettingsDialog() {
        for (const auto brush : {pageBrush, panelBrush, inputBrush}) if (brush) DeleteObject(brush);
        for (const auto font : {titleFont, sectionFont, bodyFont, smallFont, tinyFont}) if (font) DeleteObject(font);
    }
};

RECT DialogBounds(HWND dialog, int x, int y, int width, int height) {
    RECT bounds{x, y, x + width, y + height};
    MapDialogRect(dialog, &bounds);
    return bounds;
}

HWND SettingsControl(HWND dialog, const wchar_t* type, const wchar_t* text, DWORD style,
                     int id, int x, int y, int width, int height, HFONT font = nullptr) {
    const auto bounds = DialogBounds(dialog, x, y, width, height);
    const auto control = CreateWindowExW(0, type, text, WS_CHILD | WS_VISIBLE | style,
        bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top,
        dialog, reinterpret_cast<HMENU>(INT_PTR(id)), GetModuleHandleW(nullptr), nullptr);
    if (!font) font = reinterpret_cast<HFONT>(SendMessageW(dialog, WM_GETFONT, 0, 0));
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), FALSE);
    return control;
}

BOOL AppsDarkMode() {
    if (trayPalette.highContrast) return FALSE;
    if (appTheme != AppTheme::system) return appTheme == AppTheme::dark;
    return ReadNumber(L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                      L"AppsUseLightTheme", 1) == 0;
}

SettingsColors SettingsPalette(BOOL dark) {
    if (trayPalette.highContrast) {
        return {GetSysColor(COLOR_WINDOW), GetSysColor(COLOR_WINDOW), GetSysColor(COLOR_WINDOW),
                GetSysColor(COLOR_WINDOWTEXT), GetSysColor(COLOR_WINDOWTEXT), GetSysColor(COLOR_WINDOWTEXT),
                GetSysColor(COLOR_HIGHLIGHT), GetSysColor(COLOR_HIGHLIGHTTEXT), GetSysColor(COLOR_WINDOWTEXT),
                GetSysColor(COLOR_WINDOW), GetSysColor(COLOR_WINDOWTEXT), GetSysColor(COLOR_WINDOW),
                GetSysColor(COLOR_WINDOWTEXT)};
    }
    if (dark) return {RGB(26, 32, 43), RGB(31, 38, 51), RGB(23, 29, 39), RGB(50, 59, 74),
                      RGB(237, 241, 248), RGB(161, 174, 193), RGB(169, 163, 255), RGB(29, 24, 69),
                      RGB(42, 51, 66), RGB(53, 64, 82), RGB(142, 232, 239), RGB(34, 64, 71), RGB(168, 182, 203)};
    return {RGB(248, 249, 252), RGB(255, 255, 255), RGB(240, 243, 248), RGB(197, 206, 221),
            RGB(32, 41, 58), RGB(81, 95, 117), RGB(89, 72, 193), RGB(255, 255, 255),
            RGB(217, 223, 234), RGB(217, 224, 235), RGB(2, 101, 114), RGB(218, 241, 244), RGB(86, 104, 128)};
}

HICON AppIcon() {
    static const auto icon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(appIconResource));
    return icon;
}

void SetDialogChrome(HWND dialog, BOOL dark) {
    DwmSetWindowAttribute(dialog, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    const DWM_WINDOW_CORNER_PREFERENCE corners = DWMWCP_ROUND;
    DwmSetWindowAttribute(dialog, DWMWA_WINDOW_CORNER_PREFERENCE, &corners, sizeof(corners));
    if (!trayPalette.highContrast) {
        const auto colors = SettingsPalette(dark);
        DwmSetWindowAttribute(dialog, DWMWA_CAPTION_COLOR, &colors.page, sizeof(colors.page));
        DwmSetWindowAttribute(dialog, DWMWA_TEXT_COLOR, &colors.text, sizeof(colors.text));
    }
}

int SettingsScale(HWND dialog, int logical) {
    return MulDiv(logical, GetDpiForWindow(dialog), 96);
}

RECT SettingsPixels(HWND dialog, RECT rect) {
    return {SettingsScale(dialog, rect.left), SettingsScale(dialog, rect.top),
            SettingsScale(dialog, rect.right), SettingsScale(dialog, rect.bottom)};
}

HFONT SettingsPixelFont(HWND dialog, int pixels, int weight = FW_NORMAL) {
    return CreateFontW(-SettingsScale(dialog, pixels), 0, 0, 0, weight, FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, uiFont);
}

void SizeSettingsWindow(HWND dialog, bool advanced) {
    RECT frame = SettingsPixels(dialog, {0, 0, advanced ? 430 : settingsWidth, advanced ? 254 : settingsHeight});
    AdjustWindowRectExForDpi(&frame, GetWindowLongW(dialog, GWL_STYLE), FALSE,
                            GetWindowLongW(dialog, GWL_EXSTYLE), GetDpiForWindow(dialog));
    auto monitor = MonitorFromWindow(dialog, MONITOR_DEFAULTTONEAREST);
    MONITORINFO display{sizeof(display)};
    GetMonitorInfoW(monitor, &display);
    const auto& area = display.rcWork;
    const auto width = frame.right - frame.left, height = frame.bottom - frame.top;
    const auto x = area.left + std::max(0L, (area.right - area.left - width) / 2);
    const auto y = area.top + std::max(0L, (area.bottom - area.top - height) / 2);
    SetWindowPos(dialog, nullptr, x, y, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
}

void InitializeSettingsTheme(HWND dialog, SettingsDialog& state) {
    const BOOL dark = AppsDarkMode();
    state.colors = SettingsPalette(dark);
    state.defaultChargingColor = dark ? darkTray.charging : lightTray.charging;
    SetDialogChrome(dialog, dark);
    SendMessageW(dialog, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(AppIcon()));
    state.pageBrush = CreateSolidBrush(state.colors.page);
    state.panelBrush = CreateSolidBrush(state.colors.panel);
    state.inputBrush = CreateSolidBrush(state.colors.input);
    state.titleFont = SettingsPixelFont(dialog, 18, FW_SEMIBOLD);
    state.sectionFont = SettingsPixelFont(dialog, 16, FW_SEMIBOLD);
    state.bodyFont = SettingsPixelFont(dialog, 13);
    state.smallFont = SettingsPixelFont(dialog, 12);
    state.tinyFont = SettingsPixelFont(dialog, 11);
    SizeSettingsWindow(dialog, state.advanced);
}

void PaintRoundedRect(HDC dc, const RECT& bounds, COLORREF fill, COLORREF outline, int radius) {
    const HGDIOBJ brush = SelectObject(dc, GetStockObject(DC_BRUSH));
    const HGDIOBJ pen = SelectObject(dc, GetStockObject(DC_PEN));
    SetDCBrushColor(dc, fill);
    SetDCPenColor(dc, outline);
    RoundRect(dc, bounds.left, bounds.top, bounds.right, bounds.bottom, radius, radius);
    SelectObject(dc, pen);
    SelectObject(dc, brush);
}

COLORREF ColorReference(DWORD color) {
    return RGB((color >> 16) & 0xff, (color >> 8) & 0xff, color & 0xff);
}

bool IsColorButton(int id) {
    return id >= lowColor && id <= chargingColorButton;
}

DWORD SettingsSwatchColor(const SettingsDialog& state, int id) {
    if (id != chargingColorButton) return state.appearance.colors[id - lowColor];
    return state.appearance.chargingColor == themeChargingColor
        ? state.defaultChargingColor : state.appearance.chargingColor;
}

void FillSettingsRect(HDC dc, RECT bounds, COLORREF color) {
    SetDCBrushColor(dc, color);
    FillRect(dc, &bounds, reinterpret_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
}

LRESULT CALLBACK SettingsCheckboxProc(HWND box, UINT message, WPARAM wparam, LPARAM lparam,
                                      UINT_PTR id, DWORD_PTR context) {
    if (message == WM_NCDESTROY) RemoveWindowSubclass(box, SettingsCheckboxProc, id);
    if (message == WM_PAINT || message == WM_PRINTCLIENT) {
        const auto& state = *reinterpret_cast<const SettingsDialog*>(context);
        PAINTSTRUCT paint{};
        const auto dc = message == WM_PAINT ? BeginPaint(box, &paint) : reinterpret_cast<HDC>(wparam);
        const auto saved = SaveDC(dc);
        RECT bounds{};
        GetClientRect(box, &bounds);
        FillRect(dc, &bounds, state.pageBrush);
        const int size = SettingsScale(box, 15);
        const int top = (bounds.bottom - size) / 2;
        const bool checked = SendMessageW(box, BM_GETCHECK, 0, 0) == BST_CHECKED;
        const auto color = checked ? state.colors.accent : state.colors.page;
        PaintRoundedRect(dc, {0, top, size, top + size}, color,
                         checked ? color : state.colors.secondary, SettingsScale(box, 5));
        if (checked) {
            const auto pen = CreatePen(PS_SOLID, SettingsScale(box, 2), state.colors.onAccent);
            const auto previous = SelectObject(dc, pen);
            MoveToEx(dc, SettingsScale(box, 4), top + SettingsScale(box, 8), nullptr);
            LineTo(dc, SettingsScale(box, 7), top + SettingsScale(box, 11));
            LineTo(dc, SettingsScale(box, 12), top + SettingsScale(box, 5));
            SelectObject(dc, previous);
            DeleteObject(pen);
        }
        const auto previous = SelectObject(dc, state.smallFont);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, state.colors.secondary);
        wchar_t text[64]{};
        GetWindowTextW(box, text, static_cast<int>(std::size(text)));
        bounds.left = size + SettingsScale(box, 8);
        DrawTextW(dc, text, -1, &bounds, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
        if (GetFocus() == box && !(SendMessageW(box, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS)) {
            DrawTextW(dc, text, -1, &bounds, DT_SINGLELINE | DT_CALCRECT | DT_NOPREFIX);
            DrawFocusRect(dc, &bounds);
        }
        SelectObject(dc, previous);
        if (saved) RestoreDC(dc, saved);
        if (message == WM_PAINT) EndPaint(box, &paint);
        return 0;
    }
    const auto result = DefSubclassProc(box, message, wparam, lparam);
    switch (message) {
    case BM_SETCHECK: case WM_LBUTTONUP: case WM_KEYUP: case WM_SETFOCUS: case WM_KILLFOCUS:
        InvalidateRect(box, nullptr, FALSE);
        break;
    }
    return result;
}

LRESULT CALLBACK SettingsAliasProc(HWND edit, UINT message, WPARAM wparam, LPARAM lparam,
                                   UINT_PTR id, DWORD_PTR context) {
    if (message == WM_NCDESTROY) RemoveWindowSubclass(edit, SettingsAliasProc, id);
    const auto printDc = message == WM_PRINTCLIENT ? reinterpret_cast<HDC>(wparam) : nullptr;
    const auto printState = printDc ? SaveDC(printDc) : 0;
    const auto result = DefSubclassProc(edit, message, wparam, lparam);
    if (printState) RestoreDC(printDc, printState);
    if (message == WM_SETFOCUS || message == WM_KILLFOCUS) InvalidateRect(edit, nullptr, TRUE);
    if ((message == WM_PAINT || message == WM_PRINTCLIENT) && GetFocus() != edit && !GetWindowTextLengthW(edit)) {
        const auto& state = *reinterpret_cast<const SettingsDialog*>(context);
        const auto dc = message == WM_PAINT ? GetDC(edit) : printDc;
        const auto saved = SaveDC(dc);
        RECT bounds{};
        GetClientRect(edit, &bounds);
        const auto previous = SelectObject(dc, state.smallFont);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, state.colors.secondary);
        DrawTextW(dc, state.deviceName.c_str(), -1, &bounds,
                  DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
        SelectObject(dc, previous);
        if (saved) RestoreDC(dc, saved);
        if (message == WM_PAINT) ReleaseDC(edit, dc);
    }
    return result;
}

void PaintSettingsButton(const DRAWITEMSTRUCT& item, const SettingsDialog& state) {
    const auto id = static_cast<int>(item.CtlID);
    const bool accent = id == IDOK;
    const bool colorButton = IsColorButton(id);
    const bool rangeColor = colorButton && id != chargingColorButton;
    const bool quiet = id == settingsAdvanced || id == IDCANCEL;
    const auto disabled = (item.itemState & ODS_DISABLED) != 0;
    auto fill = accent ? state.colors.accent : rangeColor ? state.colors.panel :
        quiet ? state.colors.page : state.colors.input;
    if (disabled || (item.itemState & ODS_SELECTED)) fill = state.colors.panel;
    auto textColor = accent && fill == state.colors.accent ? state.colors.onAccent : state.colors.text;
    if (disabled || quiet) textColor = state.colors.secondary;
    const auto radius = SettingsScale(item.hwndItem, accent ? 14 : 12);
    if (rangeColor || quiet) FillSettingsRect(item.hDC, item.rcItem, fill);
    else PaintRoundedRect(item.hDC, item.rcItem, fill, accent && !disabled ? fill : state.colors.border, radius);
    RECT textBounds = item.rcItem;
    const auto pad = SettingsScale(item.hwndItem, 8);
    if (colorButton) {
        const auto color = ColorReference(SettingsSwatchColor(state, id));
        if (!rangeColor) {
            textBounds.left += pad;
            textBounds.right -= pad;
        }
        const auto y = (textBounds.top + textBounds.bottom - pad) / 2;
        const RECT dot{textBounds.left, y, textBounds.left + pad, y + pad};
        PaintRoundedRect(item.hDC, dot, color, color, pad);
        textBounds.left = dot.right + pad;
    }
    if (id == settingsDevice) {
        textBounds.left += pad;
        textBounds.right -= 3 * pad;
        RECT arrow = item.rcItem;
        arrow.left = arrow.right - 3 * pad;
        SetBkMode(item.hDC, TRANSPARENT);
        SetTextColor(item.hDC, state.colors.secondary);
        DrawTextW(item.hDC, L"▾", -1, &arrow, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    }
    wchar_t caption[maxAliasLength + 1]{};
    GetWindowTextW(item.hwndItem, caption, static_cast<int>(std::size(caption)));
    const auto font = reinterpret_cast<HFONT>(SendMessageW(item.hwndItem, WM_GETFONT, 0, 0));
    const auto previous = font ? SelectObject(item.hDC, font) : nullptr;
    SetBkMode(item.hDC, TRANSPARENT);
    SetTextColor(item.hDC, textColor);
    DrawTextW(item.hDC, caption, -1, &textBounds,
              (colorButton || id == settingsDevice ? DT_LEFT : DT_CENTER) |
              DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
    if ((item.itemState & ODS_FOCUS) && !(item.itemState & ODS_NOFOCUSRECT)) {
        RECT focus = item.rcItem;
        InflateRect(&focus, -3, -3);
        DrawFocusRect(item.hDC, &focus);
    }
    if (previous) SelectObject(item.hDC, previous);
}

int SettingsPercent(HWND dialog, int id) {
    wchar_t digits[5]{};
    const int length = GetDlgItemTextW(dialog, id, digits, static_cast<int>(std::size(digits)));
    if (length == 0 || length > 3 ||
        !std::all_of(digits, digits + length, [](wchar_t ch) { return ch >= L'0' && ch <= L'9'; }))
        return unknown;
    const int value = _wtoi(digits);
    return value <= 100 ? value : unknown;
}

void UpdateSettingsColorLabel(HWND dialog, int id, DWORD color) {
    if (id == chargingColorButton) {
        std::wstring caption = Text(L"Charging color · ", L"Цвет при зарядке · ");
        if (color == themeChargingColor) {
            caption += Text(L"Theme default", L"По теме");
        } else {
            wchar_t hex[8]{};
            swprintf_s(hex, L"#%06lX", color);
            caption += hex;
        }
        SetDlgItemTextW(dialog, id, caption.c_str());
    } else switch (color) {
    case 0xff909c: case 0xff7d82: SetDlgItemTextW(dialog, id, Text(L"Red", L"Красный")); break;
    case 0xf0cc76: case 0xffd85a: SetDlgItemTextW(dialog, id, Text(L"Yellow", L"Жёлтый")); break;
    case 0x83dda8: case 0x65f3b2: SetDlgItemTextW(dialog, id, Text(L"Green", L"Зелёный")); break;
    default:
        wchar_t hex[8]{};
        swprintf_s(hex, L"#%06lX", color);
        SetDlgItemTextW(dialog, id, hex);
        break;
    }
    InvalidateRect(GetDlgItem(dialog, id), nullptr, FALSE);
}

void ShowAppearance(HWND dialog, const BatteryAppearance& appearance) {
    for (size_t i = 0; i < appearance.ranges.size(); ++i) {
        SetDlgItemInt(dialog, rangeFromEdits[i], appearance.ranges[i].from, FALSE);
        SetDlgItemInt(dialog, rangeToEdits[i], appearance.ranges[i].to, FALSE);
        UpdateSettingsColorLabel(dialog, lowColor + static_cast<int>(i), appearance.colors[i]);
    }
    UpdateSettingsColorLabel(dialog, chargingColorButton, appearance.chargingColor);
}

BatteryRanges SettingsRanges(HWND dialog) {
    BatteryRanges ranges{};
    for (size_t i = 0; i < ranges.size(); ++i)
        ranges[i] = {SettingsPercent(dialog, rangeFromEdits[i]), SettingsPercent(dialog, rangeToEdits[i])};
    return ranges;
}

bool ReadSettingsRanges(HWND dialog, BatteryRanges& ranges) {
    ranges = SettingsRanges(dialog);
    const auto invalid = InvalidBatteryRange(ranges);
    if (invalid == ranges.size()) return true;

    const auto& range = ranges[invalid];
    const auto control = range.from == unknown || (range.to != unknown && range.from <= range.to)
        ? rangeFromEdits[invalid] : rangeToEdits[invalid];
    MessageBoxW(dialog, Text(L"Enter a start and end from 0 to 100% for each color. Ranges cannot overlap.",
                L"Укажи для каждого цвета начало и конец от 0 до 100%. Диапазоны не должны пересекаться."),
                Text(L"Battery ranges", L"Диапазоны заряда"), MB_OK | MB_ICONINFORMATION);
    SetFocus(GetDlgItem(dialog, control));
    return false;
}

void PickSettingsColor(HWND dialog, SettingsDialog& state, int id) {
    auto& color = id == chargingColorButton ? state.appearance.chargingColor : state.appearance.colors[id - lowColor];
    CHOOSECOLORW picker{};
    picker.lStructSize = sizeof(picker);
    picker.hwndOwner = dialog;
    picker.rgbResult = ColorReference(SettingsSwatchColor(state, id));
    picker.lpCustColors = state.customColors.data();
    picker.Flags = CC_FULLOPEN | CC_RGBINIT;
    if (ChooseColorW(&picker)) {
        color = RgbValue(picker.rgbResult);
        UpdateSettingsColorLabel(dialog, id, color);
        InvalidateRect(GetDlgItem(dialog, settingsPreview), nullptr, FALSE);
        InvalidateRect(GetDlgItem(dialog, chargePreview), nullptr, FALSE);
    }
}

UINT TrackButtonMenu(HWND dialog, int button, HMENU menu, bool alignRight = false) {
    RECT anchor{};
    GetWindowRect(GetDlgItem(dialog, button), &anchor);
    return TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | (alignRight ? TPM_RIGHTALIGN : 0),
        alignRight ? anchor.right : anchor.left, anchor.bottom, 0, dialog, nullptr);
}

void UpdateWavControl(HWND dialog, const SettingsDialog& state) {
    const auto label = std::wstring(soundLabels[static_cast<size_t>(state.alert.sound)].get()) + L" ▾";
    SetDlgItemTextW(dialog, soundSelect, label.c_str());
    const auto custom = state.alert.sound == Sound::custom;
    ShowWindow(GetDlgItem(dialog, browseWav), custom ? SW_SHOW : SW_HIDE);
    const auto filename = state.alert.file.find_last_of(L"\\/");
    const auto text = custom ? (state.alert.file.empty() ? std::wstring(Text(L"No file selected", L"Файл не выбран"))
                                                 : state.alert.file.substr(filename == std::wstring::npos ? 0 : filename + 1))
                             : std::wstring();
    SetDlgItemTextW(dialog, wavName, text.c_str());
}

void SelectNotificationSound(HWND dialog, SettingsDialog& state) {
    const auto menu = CreatePopupMenu();
    if (!menu) return;
    for (UINT i = 0; i < std::size(soundLabels); ++i)
        AppendMenuW(menu, MF_STRING | (i == static_cast<UINT>(state.alert.sound) ? MF_CHECKED : 0),
                    i + 1, soundLabels[i].get());
    const auto selected = TrackButtonMenu(dialog, soundSelect, menu);
    DestroyMenu(menu);
    if (selected) {
        state.alert.sound = static_cast<Sound>(selected - 1);
        UpdateWavControl(dialog, state);
    }
}

void OpenWavPicker(HWND dialog, SettingsDialog& state) {
    std::vector<wchar_t> filename(maxSoundPathChars + 1);
    OPENFILENAMEW picker{};
    picker.lStructSize = sizeof(picker);
    picker.hwndOwner = dialog;
    picker.lpstrTitle = notificationSoundLabel.get();
    picker.lpstrFilter = Text(L"WAV audio (*.wav)\0*.wav\0", L"Звук WAV (*.wav)\0*.wav\0");
    picker.lpstrFile = filename.data();
    picker.nMaxFile = static_cast<DWORD>(filename.size());
    picker.lpstrDefExt = L"wav";
    picker.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&picker)) return;
    const std::wstring path(filename.data());
    if (path.size() < 4 || CompareStringOrdinal(path.c_str() + path.size() - 4, 4, L".wav", 4, TRUE) != CSTR_EQUAL) {
        MessageBoxW(dialog, Text(L"Select a WAV audio file.", L"Выбери звуковой файл в формате WAV."),
                    notificationSoundLabel.get(), MB_OK | MB_ICONINFORMATION);
        return;
    }
    state.alert.file = path;
    UpdateWavControl(dialog, state);
}

HWND SettingsPixelControl(HWND dialog, const wchar_t* kind, const wchar_t* text, DWORD style,
                          int id, RECT rect, HFONT font) {
    const auto r = SettingsPixels(dialog, rect);
    const auto control = CreateWindowExW(0, kind, text, WS_CHILD | WS_VISIBLE | style,
        r.left, r.top, r.right - r.left, r.bottom - r.top, dialog,
        reinterpret_cast<HMENU>(INT_PTR(id)), GetModuleHandleW(nullptr), nullptr);
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), FALSE);
    return control;
}

std::wstring SettingsDeviceName(size_t index) {
    const auto& slot = slots[index];
    const auto selected = Select(listed, index, slot.choice);
    if (selected < listed.size()) return DisplayName(listed[selected]);
    return slot.lastSample.available() ? RememberedName(slot) : slot.label.get();
}

void UpdateSettingsPreview(HWND dialog) {
    const auto track = GetDlgItem(dialog, settingsPreview);
    if (!track) return;
    const auto level = SendMessageW(track, TBM_GETPOS, 0, 0);
    const auto text = std::to_wstring(level) + L"%";
    SetDlgItemTextW(dialog, chargePreview, text.c_str());
    const bool valid = ValidBatteryRanges(SettingsRanges(dialog));
    SetDlgItemTextW(dialog, settingsError, valid ? L"" : Text(L"Check the battery ranges.", L"Проверь границы диапазонов."));
    EnableWindow(GetDlgItem(dialog, IDOK), valid);
    InvalidateRect(track, nullptr, FALSE);
}

COLORREF SettingsPreviewColor(HWND dialog, const SettingsDialog& state) {
    auto appearance = state.appearance;
    appearance.ranges = SettingsRanges(dialog);
    const int level = static_cast<int>(SendDlgItemMessageW(dialog, settingsPreview, TBM_GETPOS, 0, 0));
    if (!ValidBatteryRanges(appearance.ranges) || BatteryBand(appearance, level) == appearance.ranges.size())
        return state.colors.neutral;
    return ColorReference(BatteryFillColor(trayPalette, {level}, appearance));
}

void PaintSettingsPreview(HWND dialog, const NMCUSTOMDRAW& draw, const SettingsDialog& state) {
    const auto track = draw.hdr.hwndFrom;
    const auto dc = draw.hdc;
    RECT client{}, channel{}, thumb{};
    GetClientRect(track, &client);
    SendMessageW(track, TBM_GETCHANNELRECT, 0, reinterpret_cast<LPARAM>(&channel));
    SendMessageW(track, TBM_GETTHUMBRECT, 0, reinterpret_cast<LPARAM>(&thumb));
    FillRect(dc, &client, state.pageBrush);
    const auto centerY = (client.top + client.bottom) / 2;
    const auto railWidth = SettingsScale(dialog, 5);
    const RECT rail{channel.left, centerY - railWidth / 2, channel.right, centerY + (railWidth + 1) / 2};
    const auto radius = SettingsScale(dialog, 6);
    const int saved = SaveDC(dc);
    const auto clip = CreateRoundRectRgn(rail.left, rail.top, rail.right, rail.bottom, radius, radius);
    ExtSelectClipRgn(dc, clip, RGN_AND);
    DeleteObject(clip);
    FillSettingsRect(dc, rail, state.colors.rail);
    const auto ranges = SettingsRanges(dialog);
    if (ValidBatteryRanges(ranges)) {
        const int width = rail.right - rail.left;
        for (size_t i = 0; i < ranges.size(); ++i) {
            const auto& range = ranges[i];
            const RECT segment{rail.left + MulDiv(range.from, width, 101), rail.top,
                               rail.left + MulDiv(range.to + 1, width, 101), rail.bottom};
            const auto color = trayPalette.highContrast ? state.colors.accent : ColorReference(state.appearance.colors[i]);
            FillSettingsRect(dc, segment, color);
        }
    }
    RestoreDC(dc, saved);
    const auto centerX = (thumb.left + thumb.right) / 2;
    const auto outer = SettingsScale(dialog, 7), inset = SettingsScale(dialog, 5);
    PaintRoundedRect(dc, {centerX - outer, centerY - outer, centerX + outer, centerY + outer},
                     state.colors.page, state.colors.text, outer * 2);
    const auto tint = SettingsPreviewColor(dialog, state);
    PaintRoundedRect(dc, {centerX - inset, centerY - inset, centerX + inset, centerY + inset},
                     tint, tint, inset * 2);
    if (GetFocus() == track) DrawFocusRect(dc, &client);
}

void PaintSettingsSurface(HWND dialog, HDC dc, const SettingsDialog& state) {
    RECT entire{};
    GetClientRect(dialog, &entire);
    FillRect(dc, &entire, state.pageBrush);
    if (state.advanced) return;
    const auto pixels = [&](RECT r) { return SettingsPixels(dialog, r); };
    for (const int y : {71, 380, 516})
        FillSettingsRect(dc, pixels({0, y, settingsWidth, y + 1}), state.colors.lineQuiet);
    PaintRoundedRect(dc, pixels(settingsRangeCard), state.colors.panel, state.colors.lineQuiet, SettingsScale(dialog, 20));
    const auto paintEdit = [&](RECT r, int id) {
        const auto focus = GetFocus() == GetDlgItem(dialog, id);
        PaintRoundedRect(dc, pixels(r), state.colors.input,
                         focus ? state.colors.accent : state.colors.border, SettingsScale(dialog, 12));
    };
    for (size_t i = 0; i < rangeFromEdits.size(); ++i) {
        paintEdit(SettingsRangeBox(i, false), rangeFromEdits[i]);
        paintEdit(SettingsRangeBox(i, true), rangeToEdits[i]);
    }
    paintEdit(settingsAliasBox, deviceAliasEdit);
    const auto cyan = state.colors.cyan;
    FillSettingsRect(dc, pixels({421, 34, 423, 40}), cyan);
    PaintRoundedRect(dc, pixels({401, 31, 421, 43}), state.colors.page, cyan, SettingsScale(dialog, 5));
    PaintRoundedRect(dc, pixels({404, 34, 416, 40}), cyan, cyan, SettingsScale(dialog, 3));
}

void CreateSettingsControls(HWND dialog, SettingsDialog& state) {
    const auto label = [&](const wchar_t* text, RECT rect, HFONT font, int id = -1, DWORD style = SS_LEFT) {
        SettingsPixelControl(dialog, L"STATIC", text, style | SS_NOPREFIX, id, rect, font);
    };
    const auto button = [&](const wchar_t* text, int id, RECT rect, HFONT font) {
        SettingsPixelControl(dialog, L"BUTTON", text, BS_OWNERDRAW | WS_TABSTOP, id, rect, font);
    };
    const auto percent = [&](int id, RECT rect) {
        rect.left += 5;
        rect.right -= 5;
        rect.top += 7;
        rect.bottom -= 6;
        SettingsPixelControl(dialog, L"EDIT", L"", ES_NUMBER | ES_CENTER | WS_TABSTOP, id, rect, state.bodyFont);
        SendDlgItemMessageW(dialog, id, EM_SETLIMITTEXT, 3, 0);
    };

    label(Text(L"Settings", L"Настройки"), {20, 24, 340, 49}, state.titleFont);
    label(appName, {430, 29, 471, 47}, state.smallFont, settingsMuted);
    label(Text(L"Battery colors", L"Цвет заряда"), {20, 90, 225, 112}, state.sectionFont);
    button(L"", chargingColorButton, {235, 83, 470, 114}, state.smallFont);
    label(Text(L"Levels outside these ranges use a neutral color.", L"Промежутки между диапазонами окрашены нейтрально."),
          {20, 116, 470, 135}, state.smallFont, settingsMuted);
    label(Text(L"Range", L"Диапазон"), {34, 159, 280, 176}, state.tinyFont, settingsMuted);
    label(Text(L"From, %", L"От, %"), {294, 159, 370, 176}, state.tinyFont, settingsMuted, SS_CENTER);
    label(Text(L"To, %", L"До, %"), {380, 159, 456, 176}, state.tinyFont, settingsMuted, SS_CENTER);
    for (size_t i = 0; i < rangeFromEdits.size(); ++i) {
        const auto from = SettingsRangeBox(i, false);
        button(L"", lowColor + static_cast<int>(i), {34, from.top, 281, from.bottom}, state.bodyFont);
        percent(rangeFromEdits[i], from);
        percent(rangeToEdits[i], SettingsRangeBox(i, true));
    }
    ShowAppearance(dialog, state.appearance);
    const auto previewLabel = Text(L"Color preview", L"Пример цвета");
    label(previewLabel, {20, 313, 281, 330}, state.smallFont, settingsMuted);
    label(L"", {388, 313, 470, 330}, state.smallFont, chargePreview, SS_RIGHT);
    INITCOMMONCONTROLSEX common{sizeof(common), ICC_BAR_CLASSES};
    InitCommonControlsEx(&common);
    const auto track = SettingsPixelControl(dialog, TRACKBAR_CLASSW, previewLabel, WS_TABSTOP | TBS_NOTICKS |
        TBS_FIXEDLENGTH, settingsPreview, {13, 330, 477, 350}, state.smallFont);
    SendMessageW(track, TBM_SETRANGE, FALSE, MAKELONG(0, 100));
    SendMessageW(track, TBM_SETTHUMBLENGTH, SettingsScale(dialog, 13), 0);
    SendMessageW(track, TBM_SETPOS, TRUE, state.battery.level == unknown ? 50 : state.battery.level);
    label(L"0%", {20, 351, 81, 365}, state.tinyFont, settingsMuted);
    label(L"100%", {406, 351, 470, 365}, state.tinyFont, settingsMuted, SS_RIGHT);

    label(Text(L"Device", L"Устройство"), {20, 400, 226, 423}, state.sectionFont);
    const auto displayName = state.alias.empty() ? state.deviceName : state.alias;
    button(displayName.c_str(), settingsDevice, {232, 395, 470, 426}, state.smallFont);
    label(Text(L"Display name", L"Имя в панели"), {20, 442, 226, 460}, state.smallFont, settingsMuted);
    const auto alias = SettingsPixelControl(dialog, L"EDIT", state.alias.c_str(), WS_TABSTOP | ES_AUTOHSCROLL,
        deviceAliasEdit, {240, 441, 462, 459}, state.smallFont);
    SendMessageW(alias, EM_SETLIMITTEXT, maxAliasLength, 0);
    SetWindowSubclass(alias, SettingsAliasProc, 0, reinterpret_cast<DWORD_PTR>(&state));
    EnableWindow(alias, !state.deviceId.empty());
    const auto sound = SettingsPixelControl(dialog, L"BUTTON", notificationSoundLabel.get(), WS_TABSTOP | BS_AUTOCHECKBOX,
        enableAlert, {20, 478, 224, 500}, state.smallFont);
    SetWindowSubclass(sound, SettingsCheckboxProc, 0, reinterpret_cast<DWORD_PTR>(&state));
    CheckDlgButton(dialog, enableAlert, state.alert.enabled ? BST_CHECKED : BST_UNCHECKED);
    button(Text(L"Advanced…", L"Дополнительно…"), settingsAdvanced, {346, 476, 470, 501}, state.smallFont);

    button(Text(L"Cancel", L"Отмена"), IDCANCEL, {20, 529, 89, 564}, state.smallFont);
    label(L"", {102, 538, 360, 560}, state.smallFont, settingsError);
    button(Text(L"Save", L"Сохранить"), IDOK, {371, 530, 470, 564}, state.bodyFont);
    SendMessageW(dialog, DM_SETDEFID, IDOK, 0);
    UpdateSettingsPreview(dialog);
}

void CreateAdvancedSettingsControls(HWND dialog, SettingsDialog& state) {
    const auto label = [&](const wchar_t* text, RECT rect, int id = settingsMuted) {
        SettingsPixelControl(dialog, L"STATIC", text, SS_NOPREFIX | SS_PATHELLIPSIS, id, rect, state.smallFont);
    };
    const auto button = [&](const wchar_t* text, int id, RECT rect) {
        SettingsPixelControl(dialog, L"BUTTON", text, BS_OWNERDRAW | WS_TABSTOP, id, rect, state.bodyFont);
    };
    SettingsPixelControl(dialog, L"STATIC", Text(L"Advanced settings", L"Дополнительные настройки"), SS_NOPREFIX, -1,
                         {20, 16, 400, 41}, state.titleFont);
    label(Text(L"Alert threshold, %", L"Порог уведомления, %"), {20, 55, 210, 76});
    SettingsPixelControl(dialog, L"EDIT", L"", ES_NUMBER | ES_CENTER | WS_TABSTOP | WS_BORDER,
                         thresholdEdit, {229, 49, 302, 77}, state.bodyFont);
    SendDlgItemMessageW(dialog, thresholdEdit, EM_SETLIMITTEXT, 3, 0);
    SetDlgItemInt(dialog, thresholdEdit, state.alert.threshold, FALSE);
    label(notificationSoundLabel.get(), {20, 92, 140, 112});
    button(L"", soundSelect, {143, 83, 304, 113});
    button(Text(L"Play", L"Прослушать"), previewSound, {310, 83, 410, 113});
    label(L"", {20, 123, 303, 144}, wavName);
    button(Text(L"WAV file…", L"Файл WAV…"), browseWav, {310, 116, 410, 145});
    UpdateWavControl(dialog, state);
    SettingsPixelControl(dialog, L"BUTTON", Text(L"Show tray icon", L"Показывать значок в трее"), WS_TABSTOP | BS_AUTOCHECKBOX,
                         showInTray, {20, 155, 250, 178}, state.smallFont);
    CheckDlgButton(dialog, showInTray, state.inTray ? BST_CHECKED : BST_UNCHECKED);
    if (AppsDarkMode()) SetWindowTheme(GetDlgItem(dialog, showInTray), L"DarkMode_Explorer", nullptr);
    button(Text(L"Reset colors", L"Сбросить цвета"), resetAppearance, {280, 150, 410, 182});
    button(Text(L"Cancel", L"Отмена"), IDCANCEL, {216, 203, 307, 237});
    button(Text(L"Done", L"Готово"), IDOK, {316, 203, 410, 237});
    SendMessageW(dialog, DM_SETDEFID, IDOK, 0);
}

std::wstring SettingsAlias(HWND dialog) {
    wchar_t alias[maxAliasLength + 1]{};
    GetDlgItemTextW(dialog, deviceAliasEdit, alias, static_cast<int>(std::size(alias)));
    const auto name = std::wstring_view(alias);
    const auto first = name.find_first_not_of(L" \t");
    if (first == name.npos) return {};
    return std::wstring(name.substr(first, name.find_last_not_of(L" \t") - first + 1));
}

bool SettingsEdited(HWND dialog, const SettingsDialog& state) {
    const auto& slot = slots[state.slot];
    const auto ranges = SettingsRanges(dialog);
    const bool sameRanges = std::equal(ranges.begin(), ranges.end(), slot.appearance.ranges.begin(),
        [](const BatteryRange& a, const BatteryRange& b) { return a.from == b.from && a.to == b.to; });
    return !sameRanges || state.appearance.colors != slot.appearance.colors ||
        state.appearance.chargingColor != slot.appearance.chargingColor || state.inTray != slot.inTray ||
        state.alert.threshold != slot.alert.threshold || state.alert.sound != slot.alert.sound ||
        state.alert.file != slot.alert.file ||
        (IsDlgButtonChecked(dialog, enableAlert) == BST_CHECKED) != slot.alert.enabled ||
        (!state.deviceId.empty() && SettingsAlias(dialog) != state.alias);
}

bool SaveSettingsDialog(HWND dialog, SettingsDialog& state) {
    BatteryRanges ranges{};
    if (!ReadSettingsRanges(dialog, ranges)) return false;
    auto alert = state.alert;
    alert.enabled = IsDlgButtonChecked(dialog, enableAlert) == BST_CHECKED;
    if (alert.enabled && alert.sound == Sound::custom && alert.file.empty()) {
        MessageBoxW(dialog, Text(L"Select a WAV file in Advanced settings.", L"Выбери WAV-файл в дополнительных настройках."),
                    notificationSoundLabel.get(), MB_OK | MB_ICONINFORMATION);
        SetFocus(GetDlgItem(dialog, settingsAdvanced));
        return false;
    }
    auto name = SettingsAlias(dialog);
    const bool aliasChanged = !state.deviceId.empty() && state.alias != name;
    if (aliasChanged && !SaveDeviceAlias(state.deviceId, name)) {
        MessageBoxW(dialog, Text(L"Couldn't save the device name.", L"Не удалось сохранить имя устройства."),
                    appName, MB_OK | MB_ICONERROR);
        return false;
    }
    auto& slot = slots[state.slot];
    state.appearance.ranges = ranges;
    if (!SavePreferences(slot, alert, state.appearance, state.inTray)) {
        if (aliasChanged) SaveDeviceAlias(state.deviceId, state.alias);
        MessageBoxW(dialog, Text(L"Couldn't save the settings.", L"Не удалось сохранить настройки."),
                    appName, MB_OK | MB_ICONERROR);
        return false;
    }
    state.alias = std::move(name);
    if (slot.alert.threshold != alert.threshold || slot.alert.enabled != alert.enabled)
        slot.alreadyAlerted = false;
    slot.alert = std::move(alert);
    slot.appearance = state.appearance;
    slot.inTray = state.inTray;
    if (aliasChanged) {
        for (auto& item : listed)
            if (DeviceIdentity(item) == state.deviceId) item.alias = state.alias;
    }
    state.saved = true;
    return true;
}

void SwitchSettingsDevice(HWND dialog, SettingsDialog& state) {
    const auto menu = CreatePopupMenu();
    if (!menu) return;
    for (size_t i = 0; i < slots.size(); ++i) {
        auto name = SettingsDeviceName(i);
        for (size_t pos = 0; (pos = name.find(L'&', pos)) != name.npos; pos += 2) name.insert(pos, 1, L'&');
        AppendMenuW(menu, MF_STRING | (i == state.slot ? MF_CHECKED : 0), i + 1, name.c_str());
    }
    const auto selected = TrackButtonMenu(dialog, settingsDevice, menu);
    DestroyMenu(menu);
    if (!selected || selected - 1 == state.slot) return;
    if (SettingsEdited(dialog, state)) {
        const auto answer = MessageBoxW(dialog, Text(L"Save changes before switching devices?",
            L"Сохранить изменения перед сменой устройства?"), appName, MB_YESNOCANCEL | MB_ICONQUESTION);
        if (answer == IDCANCEL || (answer == IDYES && !SaveSettingsDialog(dialog, state))) return;
    }
    EndDialog(dialog, settingsSwitchBase + selected - 1);
}

void OpenAdvancedSettings(HWND dialog, SettingsDialog& state);

INT_PTR CALLBACK SettingsDialogProc(HWND dialog, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_INITDIALOG) {
        auto& state = *reinterpret_cast<SettingsDialog*>(lparam);
        SetWindowLongPtrW(dialog, DWLP_USER, lparam);
        InitializeSettingsTheme(dialog, state);
        if (state.advanced) CreateAdvancedSettingsControls(dialog, state);
        else CreateSettingsControls(dialog, state);
        return TRUE;
    }
    const auto data = reinterpret_cast<SettingsDialog*>(GetWindowLongPtrW(dialog, DWLP_USER));
    if (!data) return FALSE;
    auto& state = *data;
    switch (message) {
    case WM_ERASEBKGND:
        PaintSettingsSurface(dialog, reinterpret_cast<HDC>(wparam), state);
        return TRUE;
    case WM_CTLCOLORDLG:
        return reinterpret_cast<INT_PTR>(state.pageBrush);
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX: {
        const auto dc = reinterpret_cast<HDC>(wparam);
        const auto control = reinterpret_cast<HWND>(lparam);
        const int id = GetDlgCtrlID(control);
        RECT bounds{};
        GetWindowRect(control, &bounds);
        POINT position{bounds.left, bounds.top};
        ScreenToClient(dialog, &position);
        const auto card = SettingsPixels(dialog, settingsRangeCard);
        const bool input = message == WM_CTLCOLOREDIT || (!IsWindowEnabled(control) && id == deviceAliasEdit);
        const bool inPanel = !state.advanced && PtInRect(&card, position);
        auto color = state.colors.text;
        if (id == settingsMuted || id == enableAlert || id == wavName || !IsWindowEnabled(control)) color = state.colors.secondary;
        if (id == chargePreview) color = SettingsPreviewColor(dialog, state);
        if (id == settingsError) color = ColorReference(state.appearance.colors[0]);
        SetTextColor(dc, color);
        SetBkColor(dc, input ? state.colors.input : inPanel ? state.colors.panel : state.colors.page);
        return reinterpret_cast<INT_PTR>(input ? state.inputBrush : inPanel ? state.panelBrush : state.pageBrush);
    }
    case WM_DRAWITEM:
        PaintSettingsButton(*reinterpret_cast<const DRAWITEMSTRUCT*>(lparam), state);
        return TRUE;
    case WM_NOTIFY: {
        const auto& header = *reinterpret_cast<const NMHDR*>(lparam);
        if (header.idFrom != settingsPreview || header.code != NM_CUSTOMDRAW) return FALSE;
        const auto& draw = *reinterpret_cast<const NMCUSTOMDRAW*>(lparam);
        if (draw.dwDrawStage != CDDS_PREPAINT) return FALSE;
        PaintSettingsPreview(dialog, draw, state);
        SetWindowLongPtrW(dialog, DWLP_MSGRESULT, CDRF_SKIPDEFAULT);
        return TRUE;
    }
    case WM_HSCROLL:
        if (reinterpret_cast<HWND>(lparam) != GetDlgItem(dialog, settingsPreview)) return FALSE;
        UpdateSettingsPreview(dialog);
        return TRUE;
    case WM_LBUTTONDOWN: {
        if (state.advanced) return FALSE;
        const POINT click{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        for (size_t i = 0; i < rangeFromEdits.size(); ++i) {
            for (const bool to : {false, true}) {
                const auto field = SettingsPixels(dialog, SettingsRangeBox(i, to));
                if (PtInRect(&field, click)) {
                    SetFocus(GetDlgItem(dialog, to ? rangeToEdits[i] : rangeFromEdits[i]));
                    return TRUE;
                }
            }
        }
        const auto alias = SettingsPixels(dialog, settingsAliasBox);
        if (PtInRect(&alias, click)) SetFocus(GetDlgItem(dialog, deviceAliasEdit));
        return TRUE;
    }
    case WM_NCHITTEST:
        if (!state.advanced) {
            POINT cursor{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            ScreenToClient(dialog, &cursor);
            if (cursor.y >= 0 && cursor.y < SettingsScale(dialog, 71)) {
                SetWindowLongPtrW(dialog, DWLP_MSGRESULT, HTCAPTION);
                return TRUE;
            }
        }
        return FALSE;
    case WM_CLOSE:
        EndDialog(dialog, IDCANCEL);
        return TRUE;
    case WM_COMMAND:
        break;
    default:
        return FALSE;
    }

    const int id = LOWORD(wparam);
    const int event = HIWORD(wparam);
    if (IsColorButton(id) && event == BN_CLICKED) {
        PickSettingsColor(dialog, state, id);
        return TRUE;
    }
    if (id >= lowFrom && id <= highTo) {
        if (event == EN_CHANGE) UpdateSettingsPreview(dialog);
        if (event == EN_SETFOCUS || event == EN_KILLFOCUS) InvalidateRect(dialog, nullptr, TRUE);
        return TRUE;
    }
    switch (id) {
    case deviceAliasEdit:
        if (event == EN_SETFOCUS || event == EN_KILLFOCUS) InvalidateRect(dialog, nullptr, TRUE);
        return TRUE;
    case settingsDevice:
        SwitchSettingsDevice(dialog, state);
        return TRUE;
    case settingsAdvanced:
        OpenAdvancedSettings(dialog, state);
        return TRUE;
    case resetAppearance:
        state.appearance = {};
        state.resetColors = true;
        if (state.advanced) SetDlgItemTextW(dialog, resetAppearance, Text(L"Colors reset", L"Цвета сброшены"));
        else ShowAppearance(dialog, state.appearance);
        return TRUE;
    case soundSelect:
        SelectNotificationSound(dialog, state);
        return TRUE;
    case browseWav:
        OpenWavPicker(dialog, state);
        return TRUE;
    case previewSound: {
        const auto path = SoundPath(state.alert);
        if (path.empty() || GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
            MessageBoxW(dialog, Text(L"This sound is unavailable. Select another sound or a WAV file.",
                L"Этот звук недоступен. Выбери другой звук или укажи WAV-файл."),
                notificationSoundLabel.get(), MB_OK | MB_ICONINFORMATION);
        } else {
            QueueSound(state.tray, path);
        }
        return TRUE;
    }
    case IDOK:
        if (state.advanced) {
            const int threshold = SettingsPercent(dialog, thresholdEdit);
            if (threshold == unknown) {
                MessageBoxW(dialog, Text(L"Enter an alert threshold from 0 to 100%.", L"Укажи порог уведомления от 0 до 100%."),
                            Text(L"Alert threshold", L"Порог уведомления"), MB_OK | MB_ICONINFORMATION);
                SetFocus(GetDlgItem(dialog, thresholdEdit));
                return TRUE;
            }
            state.alert.threshold = threshold;
            state.inTray = IsDlgButtonChecked(dialog, showInTray) == BST_CHECKED;
        } else if (!SaveSettingsDialog(dialog, state)) {
            return TRUE;
        }
        EndDialog(dialog, IDOK);
        return TRUE;
    case IDCANCEL:
        EndDialog(dialog, IDCANCEL);
        return TRUE;
    }
    return FALSE;
}

std::vector<WORD> SettingsDialogTemplate(bool flyout = false) {
    DLGTEMPLATE header{};
    header.style = WS_POPUP | DS_SETFONT | WS_CLIPCHILDREN;
    header.style |= flyout ? 0 : WS_SYSMENU | WS_CAPTION | DS_MODALFRAME | DS_CENTER;
    header.dwExtendedStyle = flyout ? WS_EX_TOOLWINDOW | WS_EX_CONTROLPARENT : WS_EX_DLGMODALFRAME;
    header.cx = flyout ? 294 : 334;
    header.cy = flyout ? 246 : 326;
    std::vector<WORD> dialog(sizeof(header) / sizeof(WORD));
    std::memcpy(dialog.data(), &header, sizeof(header));
    dialog.push_back(0);
    dialog.push_back(0);
    dialog.insert(dialog.end(), appName, appName + std::size(appName));
    dialog.push_back(9);
    dialog.insert(dialog.end(), uiFont, uiFont + std::size(uiFont));
    return dialog;
}

std::vector<WORD> SettingsWindowTemplate() {
    auto dialog = SettingsDialogTemplate();
    auto& header = *reinterpret_cast<DLGTEMPLATE*>(dialog.data());
    header.style = WS_POPUP | WS_BORDER | DS_SETFONT | DS_CENTER | WS_CLIPCHILDREN;
    header.dwExtendedStyle = 0;
    return dialog;
}

void OpenAdvancedSettings(HWND dialog, SettingsDialog& state) {
    SettingsDialog advanced{state.tray, state.slot, state.alert, state.appearance, state.deviceName, state.battery};
    advanced.advanced = true;
    advanced.inTray = state.inTray;
    const auto layout = SettingsDialogTemplate();
    const auto result = DialogBoxIndirectParamW(GetModuleHandleW(nullptr),
        reinterpret_cast<const DLGTEMPLATE*>(layout.data()), dialog, SettingsDialogProc,
        reinterpret_cast<LPARAM>(&advanced));
    if (result != IDOK) return;
    state.alert = std::move(advanced.alert);
    state.inTray = advanced.inTray;
    if (advanced.resetColors) {
        state.appearance = advanced.appearance;
        ShowAppearance(dialog, state.appearance);
    }
}

void EditSettings(HWND window, size_t index) {
    const auto layout = SettingsWindowTemplate();
    while (index < slots.size()) {
        const auto& slot = slots[index];
        const auto device = Select(listed, index, slot.choice);
        const auto name = device < listed.size() ? DefaultDeviceName(listed[device]) : RememberedDefaultName(slot);
        SettingsDialog state{window, index, slot.alert, slot.appearance, name, slot.shown};
        state.inTray = slot.inTray;
        if (device < listed.size()) {
            state.deviceId = DeviceIdentity(listed[device]);
            state.alias = listed[device].alias;
        } else if (slot.lastSample.available()) {
            state.deviceId = slot.lastSample.identity;
            state.alias = ReadString(deviceNamesKey, state.deviceId.c_str());
        }
        const auto result = DialogBoxIndirectParamW(GetModuleHandleW(nullptr),
            reinterpret_cast<const DLGTEMPLATE*>(layout.data()), window, SettingsDialogProc,
            reinterpret_cast<LPARAM>(&state));
        if (state.saved) {
            RepaintTrayIcon(window, index);
            Refresh(window);
        }
        if (result < settingsSwitchBase) return;
        index = static_cast<size_t>(result - settingsSwitchBase);
    }
}

constexpr int flyoutTitle = 700;
constexpr int flyoutRefresh = 701;
constexpr int flyoutTheme = 702;
constexpr int flyoutUpdated = 703;
constexpr int flyoutListTitle = 704;
constexpr int flyoutNames = 710;
constexpr int flyoutStates = 720;
constexpr int flyoutLevels = 730;
constexpr int flyoutActions = 740;
constexpr int flyoutWidth = 390;
constexpr int flyoutRowStart = 120, flyoutRowStep = 92, flyoutRowHeight = 84;
constexpr int flyoutTextLeft = 79, flyoutBatteryLeft = 269, flyoutBatteryRight = 325;
constexpr std::array<size_t, slotCount> flyoutOrder{2, 0, 1, 3};
constexpr UINT_PTR flyoutMotionTimer = 3;
constexpr UINT flyoutFrameMs = 24;
constexpr int flyoutMotionMs = 144;
HWND batteryFlyout = nullptr;
ULONGLONG lastScanAt = 0;
SYSTEMTIME lastScanClock{};

UINT PollInterval(bool flyoutVisible) {
    if (flyoutVisible) return flyoutRefreshMs;
    const bool active = std::any_of(slots.begin(), slots.end(), [](const Slot& slot) {
        const auto& battery = slot.shown;
        return battery.charging ||
            (slot.alert.enabled && !battery.stale && battery.level != unknown &&
             battery.level <= slot.alert.threshold + 5);
    });
    return active ? activeRefreshMs : idleRefreshMs;
}

void ArmPollTimer(HWND window) {
    const UINT interval = PollInterval(batteryFlyout != nullptr);
    if (!SetCoalescableTimer(window, pollTimer, interval, nullptr,
                            interval == idleRefreshMs ? 3000 : TIMERV_NO_COALESCING))
        SetTimer(window, pollTimer, interval, nullptr);
}

struct FlyoutMotion {
    ULONGLONG started = 0;
    int from = 0, to = 0;

    bool Finished(ULONGLONG now) const { return now - started >= flyoutMotionMs; }

    int Value(ULONGLONG now) const {
        if (Finished(now)) return to;
        const int remaining = flyoutMotionMs - static_cast<int>(now - started);
        return to + MulDiv(from - to, remaining * remaining, flyoutMotionMs * flyoutMotionMs);
    }
};

struct FlyoutState {
    HWND tray{};
    SettingsColors colors{};
    HBRUSH page = nullptr, panel = nullptr;
    HFONT heading = nullptr, name = nullptr, value = nullptr, small = nullptr, status = nullptr;
    HFONT label = nullptr, symbol = nullptr;
    std::array<int, slotCount> rows{-1, -1, -1, -1};
    std::array<BatteryImage, slotCount> battery{};
    std::array<FlyoutMotion, slotCount> rails{};
    FlyoutMotion reveal{};
    bool hasScan = false;
    int rowCount = -1;
    RECT anchor{}, work{};
    bool anchored = false;
    bool inMenu = false;
    bool dark = false;
    bool animate = false;
    bool animationTimer = false;
    ~FlyoutState() {
        for (auto brush : {page, panel}) if (brush) DeleteObject(brush);
        for (auto font : {heading, name, value, small, status, label, symbol}) if (font) DeleteObject(font);
    }
};

int FlyoutPx(HWND dialog, int size) {
    return MulDiv(size, std::max<UINT>(GetDpiForWindow(dialog), 96), 96);
}

RECT FlyoutRect(HWND dialog, int x, int y, int width, int height) {
    return {FlyoutPx(dialog, x), FlyoutPx(dialog, y),
            FlyoutPx(dialog, x + width), FlyoutPx(dialog, y + height)};
}

RECT FlyoutRail(HWND dialog, int row) {
    return FlyoutRect(dialog, flyoutTextLeft, flyoutRowStart + flyoutRowStep * row + 41, 180, 4);
}

void MoveFlyoutY(HWND dialog, int y) {
    RECT bounds{};
    if (GetWindowRect(dialog, &bounds) && bounds.top != y)
        SetWindowPos(dialog, nullptr, bounds.left, y, 0, 0,
            SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOOWNERZORDER);
}

bool ArmFlyoutMotion(HWND dialog, FlyoutState& state) {
    if (!state.animationTimer) state.animationTimer = SetTimer(dialog, flyoutMotionTimer, flyoutFrameMs, nullptr) != 0;
    return state.animationTimer;
}

void AdvanceFlyoutMotion(HWND dialog, FlyoutState& state, bool finish = false) {
    const auto now = GetTickCount64();
    if (state.reveal.started) {
        MoveFlyoutY(dialog, finish ? state.reveal.to : state.reveal.Value(now));
        if (finish || state.reveal.Finished(now)) state.reveal.started = 0;
    }
    bool pending = state.reveal.started != 0;
    for (size_t i = 0; i < state.rails.size(); ++i) {
        auto& motion = state.rails[i];
        if (!motion.started) continue;
        if (state.rows[i] >= 0) {
            const auto rect = FlyoutRail(dialog, state.rows[i]);
            InvalidateRect(dialog, &rect, TRUE);
        }
        if (finish || motion.Finished(now)) motion.started = 0;
        pending = pending || motion.started != 0;
    }
    if (!pending && state.animationTimer) {
        KillTimer(dialog, flyoutMotionTimer);
        state.animationTimer = false;
    }
}

void BeginFlyoutReveal(HWND dialog, FlyoutState& state) {
    if (!state.animate || !state.anchored) return;
    RECT bounds{};
    if (!GetWindowRect(dialog, &bounds)) return;
    const LONG shift = FlyoutPx(dialog, bounds.bottom <= state.anchor.top ? 6 : -6);
    const LONG limit = std::max(state.work.top, state.work.bottom - (bounds.bottom - bounds.top));
    const LONG start = std::clamp(bounds.top + shift, state.work.top, limit);
    if (start == bounds.top || !ArmFlyoutMotion(dialog, state)) return;
    state.reveal = {GetTickCount64(), static_cast<int>(start), static_cast<int>(bounds.top)};
    MoveFlyoutY(dialog, start);
}

HFONT FlyoutFont(HWND dialog, int pixels, LONG weight, const wchar_t* face = uiFont) {
    return CreateFontW(-FlyoutPx(dialog, pixels), 0, 0, 0, weight, FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, face);
}

void PositionFlyoutControl(HWND dialog, int id, int x, int y, int width, int height, bool show = true) {
    const auto box = FlyoutRect(dialog, x, y, width, height);
    SetWindowPos(GetDlgItem(dialog, id), nullptr, box.left, box.top, box.right - box.left, box.bottom - box.top,
                 SWP_NOACTIVATE | SWP_NOZORDER | (show ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
}

int FlyoutFooterY(const FlyoutState& state) {
    return std::max(1, state.rowCount) * flyoutRowStep + flyoutRowStart + 10;
}

void ResizeFlyout(HWND dialog, FlyoutState& state) {
    RECT outer{}, client{};
    GetWindowRect(dialog, &outer);
    GetClientRect(dialog, &client);
    const LONG width = FlyoutPx(dialog, flyoutWidth) + outer.right - outer.left - client.right;
    const LONG height = FlyoutPx(dialog, FlyoutFooterY(state) + 63) + outer.bottom - outer.top - client.bottom;
    LONG x = outer.left, y = outer.top;
    if (state.anchored) {
        const LONG gap = FlyoutPx(dialog, 8);
        const auto& work = state.work;
        x = std::clamp(state.anchor.right - width, work.left + gap, std::max(work.left + gap, work.right - width - gap));
        y = state.anchor.top - height - gap;
        if (y < work.top + gap) y = state.anchor.bottom + gap;
        y = std::clamp(y, work.top + gap, std::max(work.top + gap, work.bottom - height - gap));
    }
    state.reveal.started = 0;
    SetWindowPos(dialog, nullptr, x, y, width, height, SWP_NOACTIVATE | SWP_NOZORDER);
    if (!(GetWindowLongPtrW(dialog, GWL_STYLE) & WS_CAPTION)) {
        const int diameter = FlyoutPx(dialog, 32);
        if (const auto region = CreateRoundRectRgn(0, 0, width + 1, height + 1, diameter, diameter))
            if (!SetWindowRgn(dialog, region, TRUE)) DeleteObject(region);
    }
}

void ApplyFlyoutTheme(HWND dialog, FlyoutState& state) {
    const BOOL dark = AppsDarkMode();
    BOOL animations = FALSE;
    state.animate = !trayPalette.highContrast &&
        SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animations, 0) && animations;
    if (!state.animate) AdvanceFlyoutMotion(dialog, state, true);
    state.dark = dark != FALSE;
    state.colors = SettingsPalette(dark);
    if (state.page) DeleteObject(state.page);
    if (state.panel) DeleteObject(state.panel);
    state.page = CreateSolidBrush(state.colors.page);
    state.panel = CreateSolidBrush(state.colors.panel);
    SetDialogChrome(dialog, dark);
    RedrawWindow(dialog, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

FlyoutState* FlyoutWindowState(HWND dialog) {
    return reinterpret_cast<FlyoutState*>(GetWindowLongPtrW(dialog, DWLP_USER));
}

UINT TrackFlyoutMenu(HWND dialog, HMENU menu) {
    UINT selected = 0;
    if (auto* state = FlyoutWindowState(dialog)) {
        state->inMenu = true;
        selected = TrackButtonMenu(dialog, flyoutTheme, menu, true);
    }
    DestroyMenu(menu);
    // TrackPopupMenu runs a nested message loop which can destroy the flyout.
    if (auto* state = FlyoutWindowState(dialog)) {
        state->inMenu = false;
        return selected;
    }
    return 0;
}

void SelectTrayIcons(HWND dialog);
void UpdateBatteryFlyout();

void ApplyFlyoutLanguage(HWND dialog) {
    SetDlgItemTextW(dialog, flyoutListTitle, Text(L"Devices", L"Устройства"));
    SetDlgItemTextW(dialog, flyoutTheme, Text(L"Theme, tray icons, and language", L"Тема, значки в трее и язык"));
    SetDlgItemTextW(dialog, flyoutRefresh, refreshLabel.get());
    for (size_t i = 0; i < slots.size(); ++i) {
        const auto label = std::wstring(Text(L"Settings · ", L"Настройки · ")) + slots[i].label.get();
        SetDlgItemTextW(dialog, flyoutActions + static_cast<int>(i), label.c_str());
    }
}

void UpdateInterfaceLanguage(HWND dialog, HWND tray) {
    ApplyFlyoutLanguage(dialog);
    for (size_t i = 0; i < slots.size(); ++i) {
        const auto& slot = slots[i];
        const auto selected = Select(listed, i, slot.choice);
        if (selected < listed.size()) {
            ShowStatus(tray, i, slot.shown, TrayDeviceDetail(listed[selected], slot.shown));
        } else {
            ShowRememberedStatus(tray, i);
        }
    }
    UpdateBatteryFlyout();
}

void ShowFlyoutMenu(HWND dialog) {
    const auto menu = CreatePopupMenu();
    if (!menu) return;
    constexpr LocalizedText themes[]{{L"Use Windows setting", L"Как в Windows"}, {L"Light", L"Светлая"}, {L"Dark", L"Тёмная"}};
    for (UINT i = 0; i < std::size(themes); ++i)
        AppendMenuW(menu, MF_STRING | (i == static_cast<UINT>(appTheme) ? MF_CHECKED : 0), i + 1, themes[i].get());
    constexpr UINT trayIcons = 4;
    constexpr UINT firstLanguage = 5;
    constexpr const wchar_t* languages[]{L"English", L"Русский"};
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, trayIcons, Text(L"Tray icons…", L"Значки в трее…"));
    if (const auto languageMenu = CreatePopupMenu()) {
        for (UINT i = 0; i < std::size(languages); ++i) {
            AppendMenuW(languageMenu, MF_STRING | (i == static_cast<UINT>(g_language) ? MF_CHECKED : 0),
                        firstLanguage + i, languages[i]);
        }
        if (!AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(languageMenu), Text(L"Language", L"Язык")))
            DestroyMenu(languageMenu);
    }
    const auto selected = TrackFlyoutMenu(dialog, menu);
    auto* state = FlyoutWindowState(dialog);
    if (!state) return;
    if (selected == trayIcons) {
        SelectTrayIcons(dialog);
    } else if (selected >= firstLanguage && selected < firstLanguage + std::size(languages)) {
        const auto language = static_cast<AppLanguage>(selected - firstLanguage);
        if (language != g_language && WriteNumber(settingsKey, L"Language", static_cast<DWORD>(language))) {
            g_language = language;
            UpdateInterfaceLanguage(dialog, state->tray);
        }
    } else if (selected && selected <= std::size(themes) && WriteNumber(settingsKey, L"Theme", selected - 1)) {
        appTheme = static_cast<AppTheme>(selected - 1);
        UpdateTrayPalette(state->tray);
        ApplyFlyoutTheme(dialog, *state);
    }
}

void SelectTrayIcons(HWND dialog) {
    const auto menu = CreatePopupMenu();
    if (!menu) return;
    for (UINT i = 0; i < slots.size(); ++i)
        AppendMenuW(menu, MF_STRING | (slots[i].inTray ? MF_CHECKED : 0), i + 1, slots[i].label.get());
    const auto selected = TrackFlyoutMenu(dialog, menu);
    const auto* state = FlyoutWindowState(dialog);
    if (!state || !selected || selected > slots.size()) return;
    auto& slot = slots[selected - 1];
    if (SavePreferences(slot, slot.alert, slot.appearance, !slot.inTray)) {
        slot.inTray = !slot.inTray;
        RepaintTrayIcon(state->tray, selected - 1);
    }
}

std::wstring DeviceStatus(const Device& device, const BatteryImage& battery) {
    std::wstring text = ConnectionLabel(device.connection);
    const auto add = [&](const wchar_t* state) {
        if (!text.empty()) text += L" · ";
        text += state;
    };
    if (battery.charging) add(Text(L"Charging", L"Заряжается"));
    if (battery.stale) add(lastReadingLabel.get());
    else if (battery.level == unknown && !battery.charging) add(Text(L"Battery unavailable", L"Заряд недоступен"));
    if (text.empty()) text = Text(L"Connected", L"Подключено");
    return text;
}

bool SetFlyoutText(int id, const std::wstring& text) {
    const auto control = GetDlgItem(batteryFlyout, id);
    wchar_t previous[192]{};
    const auto copied = GetWindowTextW(control, previous, std::size(previous));
    if (std::wstring_view(previous, copied) == text) return false;
    SetWindowTextW(control, text.c_str());
    return true;
}

void UpdateBatteryFlyout() {
    if (!batteryFlyout) return;
    auto& view = *reinterpret_cast<FlyoutState*>(GetWindowLongPtrW(batteryFlyout, DWLP_USER));
    const bool hasScan = lastScanAt != 0;
    bool repaint = std::exchange(view.hasScan, hasScan) != hasScan;
    int rowCount = 0;
    std::array<int, slotCount> rows{};
    for (const size_t i : flyoutOrder) {
        const auto& slot = slots[i];
        const auto selected = Select(listed, i, slot.choice);
        const bool connected = selected < listed.size();
        const bool remembered = slot.lastSample.available();
        // Keep a way to open gamepad settings even before its first battery reading.
        rows[i] = i == gamepadSlot || connected || remembered ? rowCount++ : -1;
        std::wstring name = slot.label.get();
        std::wstring state = scanning ? checkingLabel.get() : Text(L"Disconnected", L"Не подключено");
        if (connected) {
            name = DisplayName(listed[selected]);
            state = DeviceStatus(listed[selected], slot.shown);
        } else if (remembered) {
            name = RememberedName(slot);
            state = slot.lastSample.level == unknown ? Text(L"No response", L"Нет ответа") : lastReadingLabel.get();
            const std::wstring_view route = ConnectionLabel(slot.lastSample.connection);
            if (!route.empty()) state.append(L" · ").append(route);
        }
        const auto level = slot.shown.level == unknown ? std::wstring(L"—") : std::to_wstring(slot.shown.level) + L"%";
        const bool nameChanged = SetFlyoutText(flyoutNames + static_cast<int>(i), name);
        SetFlyoutText(flyoutStates + static_cast<int>(i), state);
        SetFlyoutText(flyoutLevels + static_cast<int>(i), level);
        if (view.battery[i] != slot.shown) {
            const auto& before = view.battery[i];
            auto& motion = view.rails[i];
            if (view.animate && !nameChanged && rows[i] >= 0 && IsWindowVisible(batteryFlyout) &&
                !before.stale && !slot.shown.stale && before.level >= 0 && slot.shown.level >= 0 &&
                before.level != slot.shown.level && ArmFlyoutMotion(batteryFlyout, view)) {
                const auto now = GetTickCount64();
                motion = {now, motion.started ? motion.Value(now) : before.level, slot.shown.level};
            } else if (before.level != slot.shown.level || slot.shown.stale) {
                motion.started = 0;
            }
            view.battery[i] = slot.shown;
            repaint = true;
        }
    }
    if (rowCount != view.rowCount || rows != view.rows) {
        repaint = true;
        const bool resize = rowCount != view.rowCount;
        view.rows = rows;
        view.rowCount = rowCount;
        for (size_t i = 0; i < rows.size(); ++i) {
            const int index = static_cast<int>(i);
            const bool visible = rows[i] >= 0;
            const int y = flyoutRowStart + std::max(0, rows[i]) * flyoutRowStep;
            PositionFlyoutControl(batteryFlyout, flyoutNames + index, flyoutTextLeft, y + 13, 180, 22, visible);
            PositionFlyoutControl(batteryFlyout, flyoutStates + index, flyoutTextLeft, y + 50, 182, 16, visible);
            PositionFlyoutControl(batteryFlyout, flyoutLevels + index, flyoutBatteryLeft, y + 19,
                                  flyoutBatteryRight - flyoutBatteryLeft, 45, visible);
            PositionFlyoutControl(batteryFlyout, flyoutActions + index, 334, y + 28, 25, 28, visible);
        }
        const int footer = FlyoutFooterY(view);
        PositionFlyoutControl(batteryFlyout, flyoutUpdated, 32, footer + 21, 200, 20);
        PositionFlyoutControl(batteryFlyout, flyoutRefresh, 266, footer + 16, 104, 30);
        if (resize) ResizeFlyout(batteryFlyout, view);
    }
    wchar_t time[20]{};
    const wchar_t* updated = Text(L"Waiting for data", L"Ожидание данных");
    if (lastScanAt != 0 && GetTickCount64() - lastScanAt < 30000) updated = Text(L"Just now", L"Только что");
    else if (lastScanAt != 0 && GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS,
                                                &lastScanClock, nullptr, time, std::size(time)))
        updated = time;
    SetFlyoutText(flyoutUpdated, updated);
    if (repaint) RedrawWindow(batteryFlyout, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

template<int samples = 4, class Shape>
void DrawGlyph(HDC dc, const RECT& rect, COLORREF color, COLORREF background, Shape shape) {
    if (!RectVisible(dc, &rect)) return;
    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;
    if (width <= 0 || height <= 0) return;
    constexpr int area = samples * samples;
    std::vector<DWORD> bitmap(static_cast<size_t>(width) * height);
    const auto mix = [](int fore, int back, int covered) -> DWORD {
        return (fore * covered + back * (area - covered) + area / 2) / area;
    };
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            int covered = 0;
            for (int sy = 0; sy < samples; ++sy)
                for (int sx = 0; sx < samples; ++sx)
                    covered += shape((x + (sx + 0.5f) / samples) / width,
                                     (y + (sy + 0.5f) / samples) / height);
            const auto red = mix(GetRValue(color), GetRValue(background), covered);
            const auto green = mix(GetGValue(color), GetGValue(background), covered);
            const auto blue = mix(GetBValue(color), GetBValue(background), covered);
            bitmap[y * width + x] = (red << 16) | (green << 8) | blue;
        }
    }
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    SetDIBitsToDevice(dc, rect.left, rect.top, width, height, 0, 0, 0, height,
                      bitmap.data(), &info, DIB_RGB_COLORS);
}

COLORREF FlyoutChargeColor(const FlyoutState& state, const Slot& slot, bool forText = false) {
    if (trayPalette.highContrast) return state.colors.text;
    if (slot.shown.charging) {
        if (forText && slot.appearance.chargingColor != themeChargingColor) return state.colors.text;
        return ColorReference(ChargingColor(state.dark ? darkTray : lightTray, slot.appearance));
    }
    if (slot.shown.level == unknown) return state.colors.neutral;
    const size_t band = BatteryBand(slot.appearance, slot.shown.level);
    if (band >= slot.appearance.colors.size()) return state.colors.neutral;
    const DWORD color = slot.appearance.colors[band];
    if (forText && !state.dark && band == 1 && IsFactoryBatteryColor(band, color)) return state.colors.text;
    return ColorReference(state.dark ? color : LightBatteryTone(band, color));
}

COLORREF BlendFlyoutColors(COLORREF first, COLORREF second) {
    return RGB((GetRValue(first) * 7 + GetRValue(second) * 3) / 10,
               (GetGValue(first) * 7 + GetGValue(second) * 3) / 10,
               (GetBValue(first) * 7 + GetBValue(second) * 3) / 10);
}

bool FlyoutBatteryMark(float x, float y) {
    x *= 23; y *= 12;
    return std::abs(RoundedBox(x, y, 10, 6, 8.8f, 4.8f, 2.6f)) <= .72f ||
        RoundedBox(x, y, 9, 6, 4.4f, 1.9f, .6f) <= 0 ||
        RoundedBox(x, y, 21.3f, 6, .8f, 2, .55f) <= 0;
}

bool FlyoutDeviceGlyph(size_t index, float x, float y) {
    x = x * 24 - 12;
    y = y * 24 - 12;
    if (index == 0) {
        const bool outline = std::abs(RoundedBox(x, y, 0, .2f, 6.05f, 8.65f, 5.0f)) <= .65f;
        const bool wheel = RoundedBox(x, y, 0, -4.5f, .78f, 1.46f, .62f) <= 0;
        const bool seam = RoundedBox(x, y, 0, -7.33f, .36f, .87f, .30f) <= 0 ||
            RoundedBox(std::abs(x), y, 3.75f, -1.50f, 1.93f, .41f, .36f) <= 0;
        return outline || wheel || seam;
    }
    if (index == 1) {
        const float side = std::abs(x), bandY = y + .95f;
        const float radiusSquared = side * side + bandY * bandY;
        const bool band = bandY <= 0 && radiusSquared >= 7.05f * 7.05f && radiusSquared <= 8.5f * 8.5f;
        const bool arms = RoundedBox(side, y, 7.79f, .1f, .66f, 1.30f, .52f) <= 0;
        const bool cups = std::abs(RoundedBox(side, y, 6.63f, 4.55f, 2.49f, 3.46f, 1.56f)) <= .64f;
        const bool cushions = RoundedBox(side, y, 5.3f, 4.58f, .40f, 1.89f, .36f) <= 0;
        return band || arms || cups || cushions;
    }
    if (index == gamepadSlot) {
        const float side = std::abs(x);
        const float body = RoundedBox(x, y, 0, -1.9f, 8.8f, 4.24f, 3.2f);
        const float grips = RoundedBox(side - (y - .2f) * .32f, y, 6.65f, 2.27f, 2.72f, 4.17f, 2.25f);
        if (std::abs(std::min(body, grips)) <= .66f) return true;
        const float padX = std::abs(x + 4.5f), padY = std::abs(y + .85f);
        if ((padX <= .55f && padY <= 1.63f) || (padX <= 1.63f && padY <= .55f)) return true;
        const float buttonX = std::abs(x - 4.55f), buttonY = std::abs(y + .85f);
        const float horizontal = buttonX - 1.37f, vertical = buttonY - 1.37f;
        return horizontal * horizontal + buttonY * buttonY <= .64f * .64f ||
               vertical * vertical + buttonX * buttonX <= .64f * .64f ||
               RoundedBox(side, y, 1.0f, -2.85f, .47f, .38f, .32f) <= 0;
    }
    if (std::abs(RoundedBox(x, y, 0, 0, 9.95f, 5.86f, 1.76f)) <= .62f ||
        RoundedBox(x, y, 0, 3.20f, 4.04f, .60f, .40f) <= 0 ||
        RoundedBox(std::abs(x), y, 6.88f, 3.20f, 1.22f, .60f, .40f) <= 0) return true;
    const float side = std::abs(x);
    const float keyX = std::min({side, std::abs(side - 3.44f), std::abs(side - 6.88f)});
    return RoundedBox(keyX, std::abs(y + 1.55f), 0, 1.21f, .98f, .63f, .25f) <= 0;
}

void PaintFlyoutRows(HWND dialog, HDC dc, const FlyoutState& state) {
    const int footer = FlyoutFooterY(state);
    PaintRoundedRect(dc, FlyoutRect(dialog, 0, 0, flyoutWidth, footer + 63), state.colors.page, state.colors.border,
                     FlyoutPx(dialog, 32));
    const auto mark = FlyoutRect(dialog, 21, 18, 36, 36);
    PaintRoundedRect(dc, mark, state.colors.cyanTile, state.colors.cyanTile, FlyoutPx(dialog, 18));
    DrawGlyph(dc, FlyoutRect(dialog, 29, 30, 23, 12), state.colors.cyan, state.colors.cyanTile, FlyoutBatteryMark);

    const auto divider = SelectObject(dc, GetStockObject(DC_PEN));
    SetDCPenColor(dc, state.colors.lineQuiet);
    for (const int y : {72, footer}) {
        MoveToEx(dc, 1, FlyoutPx(dialog, y), nullptr);
        LineTo(dc, FlyoutPx(dialog, flyoutWidth) - 1, FlyoutPx(dialog, y));
    }
    SelectObject(dc, divider);
    const auto dot = lastScanAt ? state.colors.cyan : state.colors.secondary;
    PaintRoundedRect(dc, FlyoutRect(dialog, 21, footer + 29, 5, 5), dot, dot, FlyoutPx(dialog, 5));

    const auto radius = FlyoutPx(dialog, 20);
    for (size_t i = 0; i < state.rows.size(); ++i) {
        if (state.rows[i] < 0) continue;
        const int y = flyoutRowStart + flyoutRowStep * state.rows[i];
        const auto& slot = slots[i];
        PaintRoundedRect(dc, FlyoutRect(dialog, 20, y, 350, flyoutRowHeight), state.colors.panel,
                         state.colors.lineQuiet, radius);
        const auto tile = FlyoutRect(dialog, 33, y + 24, 36, 36);
        PaintRoundedRect(dc, tile, state.colors.input, state.colors.input, FlyoutPx(dialog, 18));
        DrawGlyph<6>(dc, FlyoutRect(dialog, 34, y + 25, 34, 34), state.colors.secondary, state.colors.input,
                     [i](float x, float y) { return FlyoutDeviceGlyph(i, x, y); });
        const auto rail = FlyoutRail(dialog, state.rows[i]);
        const auto railRadius = FlyoutPx(dialog, 4);
        const auto& motion = state.rails[i];
        const int level = motion.started ? motion.Value(GetTickCount64()) : slot.shown.level;
        const auto tone = FlyoutChargeColor(state, slot);
        const auto outline = slot.shown.charging && level <= 0 ? tone : state.colors.rail;
        PaintRoundedRect(dc, rail, state.colors.rail, outline, railRadius);
        if (level <= 0) continue;
        RECT fill = rail;
        fill.right = fill.left + MulDiv(rail.right - rail.left, level, 100);
        if (fill.right <= fill.left) continue;
        const auto fillColor = slot.shown.stale && !slot.shown.charging ? BlendFlyoutColors(tone, state.colors.rail) : tone;
        PaintRoundedRect(dc, fill, fillColor, fillColor, railRadius);
    }
}

void PaintFlyoutLevel(const DRAWITEMSTRUCT& item, const FlyoutState& state) {
    const auto& slot = slots[item.CtlID - flyoutLevels];
    const auto dc = item.hDC;
    FillRect(dc, &item.rcItem, state.panel);
    const auto saved = SaveDC(dc);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, FlyoutChargeColor(state, slot, true));
    wchar_t text[8]{};
    const int length = GetWindowTextW(item.hwndItem, text, std::size(text));
    const auto& bounds = item.rcItem;
    SelectObject(dc, state.value);
    if (length > 1 && text[length - 1] == L'%') {
        TEXTMETRICW metrics{};
        GetTextMetricsW(dc, &metrics);
        const int baseline = bounds.top + (bounds.bottom - bounds.top - metrics.tmHeight) / 2 + metrics.tmAscent;
        SelectObject(dc, state.small);
        SIZE suffix{};
        GetTextExtentPoint32W(dc, L"%", 1, &suffix);
        SetTextAlign(dc, TA_BASELINE | TA_RIGHT | TA_NOUPDATECP);
        TextOutW(dc, bounds.right, baseline, L"%", 1);
        SelectObject(dc, state.value);
        TextOutW(dc, bounds.right - suffix.cx - FlyoutPx(item.hwndItem, 2), baseline, text, length - 1);
    } else {
        RECT textBounds = bounds;
        DrawTextW(dc, text, -1, &textBounds, DT_RIGHT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
    }
    RestoreDC(dc, saved);
}

void PaintFlyoutAction(const DRAWITEMSTRUCT& item, const FlyoutState& state) {
    const bool refresh = item.CtlID == flyoutRefresh;
    const bool onPage = refresh || item.CtlID == flyoutTheme;
    const bool pressed = (item.itemState & ODS_SELECTED) != 0;
    FillRect(item.hDC, &item.rcItem, onPage ? state.page : state.panel);
    if (item.CtlID == flyoutTheme || pressed)
        PaintRoundedRect(item.hDC, item.rcItem, pressed ? state.colors.input :
                         onPage ? state.colors.page : state.colors.panel, state.colors.border, FlyoutPx(item.hwndItem, 14));
    const auto previous = SelectObject(item.hDC, refresh ? state.label : state.symbol);
    auto bounds = item.rcItem;
    wchar_t label[48]{};
    if (refresh) GetWindowTextW(item.hwndItem, label, std::size(label));
    const auto caption = refresh ? std::wstring(L"↻  ") + label :
        item.CtlID == flyoutTheme ? (state.dark ? L"☼" : L"☾") : L"⚙";
    SetBkMode(item.hDC, TRANSPARENT);
    SetTextColor(item.hDC, state.colors.secondary);
    DrawTextW(item.hDC, caption.c_str(), -1, &bounds,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    if ((item.itemState & ODS_FOCUS) && !(item.itemState & ODS_NOFOCUSRECT)) {
        InflateRect(&bounds, -2, -2);
        DrawFocusRect(item.hDC, &bounds);
    }
    if (previous) SelectObject(item.hDC, previous);
}

INT_PTR CALLBACK BatteryFlyoutProc(HWND dialog, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_INITDIALOG) {
        auto state = std::make_unique<FlyoutState>();
        state->tray = reinterpret_cast<HWND>(lparam);
        state->heading = FlyoutFont(dialog, 22, 650);
        state->name = FlyoutFont(dialog, 15, 620);
        state->value = FlyoutFont(dialog, 24, FW_SEMIBOLD);
        state->small = FlyoutFont(dialog, 12, 650);
        state->status = FlyoutFont(dialog, 11, FW_NORMAL);
        state->label = FlyoutFont(dialog, 13, FW_NORMAL);
        state->symbol = FlyoutFont(dialog, 16, FW_NORMAL, L"Segoe UI Symbol");
        SetWindowLongPtrW(dialog, DWLP_USER, reinterpret_cast<LONG_PTR>(state.release()));
        auto& theme = *reinterpret_cast<FlyoutState*>(GetWindowLongPtrW(dialog, DWLP_USER));
        ApplyFlyoutTheme(dialog, theme);
        SettingsControl(dialog, L"STATIC", appName, SS_NOPREFIX | SS_CENTERIMAGE,
                        flyoutTitle, 0, 0, 1, 1, theme.heading);
        PositionFlyoutControl(dialog, flyoutTitle, 68, 19, 240, 33);
        SettingsControl(dialog, L"STATIC", L"", SS_NOPREFIX,
                        flyoutListTitle, 0, 0, 1, 1, theme.label);
        PositionFlyoutControl(dialog, flyoutListTitle, 20, 92, 250, 20);
        SettingsControl(dialog, L"BUTTON", L"", BS_OWNERDRAW | WS_TABSTOP,
                        flyoutTheme, 0, 0, 1, 1, theme.symbol);
        PositionFlyoutControl(dialog, flyoutTheme, 339, 20, 31, 31);
        for (const size_t i : flyoutOrder) {
            const int index = static_cast<int>(i);
            SettingsControl(dialog, L"STATIC", slots[i].label.get(), SS_ENDELLIPSIS | SS_NOPREFIX,
                            flyoutNames + index, 0, 0, 1, 1, theme.name);
            SettingsControl(dialog, L"STATIC", L"", SS_ENDELLIPSIS | SS_NOPREFIX,
                            flyoutStates + index, 0, 0, 1, 1, theme.status);
            SettingsControl(dialog, L"STATIC", L"—", SS_OWNERDRAW,
                            flyoutLevels + index, 0, 0, 1, 1, theme.value);
            SettingsControl(dialog, L"BUTTON", L"", BS_OWNERDRAW | WS_TABSTOP,
                            flyoutActions + index, 0, 0, 1, 1, theme.symbol);
        }
        SettingsControl(dialog, L"STATIC", L"", SS_NOPREFIX | SS_CENTERIMAGE,
                        flyoutUpdated, 0, 0, 1, 1, theme.status);
        SettingsControl(dialog, L"BUTTON", L"", BS_OWNERDRAW | WS_TABSTOP,
                        flyoutRefresh, 0, 0, 1, 1, theme.label);
        ApplyFlyoutLanguage(dialog);
        return TRUE;
    }
    const auto state = reinterpret_cast<FlyoutState*>(GetWindowLongPtrW(dialog, DWLP_USER));
    if (!state) return FALSE;
    switch (message) {
    case WM_ERASEBKGND:
        PaintFlyoutRows(dialog, reinterpret_cast<HDC>(wparam), *state);
        return TRUE;
    case WM_CTLCOLORDLG:
        return reinterpret_cast<INT_PTR>(state->page);
    case WM_CTLCOLORSTATIC: {
        const auto dc = reinterpret_cast<HDC>(wparam);
        const int id = GetDlgCtrlID(reinterpret_cast<HWND>(lparam));
        const bool onPage = id == flyoutTitle || id == flyoutListTitle || id == flyoutUpdated;
        const bool secondary = id == flyoutListTitle || id == flyoutUpdated || (id >= flyoutStates && id < flyoutLevels);
        SetTextColor(dc, secondary ? state->colors.secondary : state->colors.text);
        SetBkColor(dc, onPage ? state->colors.page : state->colors.panel);
        return reinterpret_cast<INT_PTR>(onPage ? state->page : state->panel);
    }
    case WM_DRAWITEM: {
        const auto& item = *reinterpret_cast<const DRAWITEMSTRUCT*>(lparam);
        if (item.CtlID >= flyoutLevels && item.CtlID < flyoutLevels + slots.size())
            PaintFlyoutLevel(item, *state);
        else PaintFlyoutAction(item, *state);
        return TRUE;
    }
    case WM_ACTIVATE:
        if (LOWORD(wparam) == WA_INACTIVE && !state->inMenu && IsWindowVisible(dialog))
            PostMessageW(dialog, WM_CLOSE, 0, 0);
        return FALSE;
    case WM_THEMECHANGED:
    case WM_SYSCOLORCHANGE:
        ApplyFlyoutTheme(dialog, *state);
        return TRUE;
    case WM_TIMER:
        if (wparam != flyoutMotionTimer) return FALSE;
        AdvanceFlyoutMotion(dialog, *state);
        return TRUE;
    case WM_COMMAND: {
        const int id = LOWORD(wparam);
        if (id == flyoutRefresh) {
            SetDlgItemTextW(dialog, flyoutUpdated, checkingLabel.get());
            Refresh(state->tray);
            return TRUE;
        }
        if (id == flyoutTheme) {
            ShowFlyoutMenu(dialog);
            return TRUE;
        }
        if (id == IDCANCEL) {
            DestroyWindow(dialog);
            return TRUE;
        }
        if (id >= flyoutActions && id < flyoutActions + static_cast<int>(slots.size())) {
            const auto tray = state->tray;
            DestroyWindow(dialog);
            EditSettings(tray, static_cast<size_t>(id - flyoutActions));
            return TRUE;
        }
        return FALSE;
    }
    case WM_CLOSE:
        DestroyWindow(dialog);
        return TRUE;
    case WM_NCDESTROY:
        if (state->animationTimer) KillTimer(dialog, flyoutMotionTimer);
        if (batteryFlyout == dialog) batteryFlyout = nullptr;
        if (state->tray) ArmPollTimer(state->tray);
        SetWindowLongPtrW(dialog, DWLP_USER, 0);
        delete state;
        return FALSE;
    }
    return FALSE;
}

void ShowBatteryFlyout(HWND tray, size_t index) {
    if (batteryFlyout) {
        DestroyWindow(batteryFlyout);
        return;
    }
    const auto layout = SettingsDialogTemplate(true);
    batteryFlyout = CreateDialogIndirectParamW(GetModuleHandleW(nullptr),
        reinterpret_cast<const DLGTEMPLATE*>(layout.data()), tray, BatteryFlyoutProc,
        reinterpret_cast<LPARAM>(tray));
    if (!batteryFlyout) return;
    RECT anchor{};
    NOTIFYICONIDENTIFIER identifier{};
    identifier.cbSize = sizeof(identifier);
    identifier.hWnd = tray;
    identifier.uID = static_cast<UINT>(index + 1);
    if (FAILED(Shell_NotifyIconGetRect(&identifier, &anchor))) {
        POINT pointer{};
        GetCursorPos(&pointer);
        anchor = {pointer.x, pointer.y, pointer.x, pointer.y};
    }
    MONITORINFO monitor{};
    monitor.cbSize = sizeof(monitor);
    if (!GetMonitorInfoW(MonitorFromRect(&anchor, MONITOR_DEFAULTTONEAREST), &monitor))
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &monitor.rcWork, 0);
    auto& view = *reinterpret_cast<FlyoutState*>(GetWindowLongPtrW(batteryFlyout, DWLP_USER));
    view.anchor = anchor;
    view.work = monitor.rcWork;
    view.anchored = true;
    UpdateBatteryFlyout();
    BeginFlyoutReveal(batteryFlyout, view);
    SetWindowPos(batteryFlyout, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW | SWP_NOOWNERZORDER);
    SetForegroundWindow(batteryFlyout);
    ArmPollTimer(tray);
    if (!scanning && (lastScanAt == 0 || GetTickCount64() - lastScanAt >= flyoutRefreshMs))
        Refresh(tray);
}

std::wstring MenuText(const std::wstring& name) {
    std::wstring text;
    for (wchar_t ch : name) {
        if (ch == L'&') text += L'&';
        text += ch;
    }
    return text;
}

void ShowMenu(HWND window, size_t index) {
    auto& slot = slots[index];
    HMENU menu = CreatePopupMenu();
    HMENU devices = CreatePopupMenu();
    if (!menu || !devices) {
        if (menu) DestroyMenu(menu);
        if (devices) DestroyMenu(devices);
        return;
    }
    AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, MenuText(slot.tip).c_str());
    const auto& choice = ChoiceForSlot(listed, index, slot.choice);
    AppendMenuW(devices, MF_STRING | (choice.empty() ? MF_CHECKED : 0), 10, Text(L"Automatic", L"Автоматически"));
    std::vector<Device> menuDevices;
    std::copy_if(listed.begin(), listed.end(), std::back_inserter(menuDevices),
                 [&](const Device& device) { return device.connected() && FitsSlot(device, index); });
    if (menuDevices.empty()) AppendMenuW(devices, MF_STRING | MF_GRAYED, 0, noConnectedDevices.get());
    for (size_t i = 0; i < menuDevices.size(); ++i) {
        const auto& device = menuDevices[i];
        auto name = MenuText(DisplayName(device));
        if (std::count_if(menuDevices.begin(), menuDevices.end(), [&](const Device& other) {
            return DisplayName(other) == DisplayName(device);
        }) > 1) name += L" (" + device.address + L")";
        AppendMenuW(devices, MF_STRING | (MatchesChoice(device, choice) ? MF_CHECKED : 0),
                    100 + i, name.c_str());
    }
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(devices), Text(L"Device", L"Устройство"));
    AppendMenuW(menu, MF_STRING, 3, Text(L"Settings…", L"Настройки…"));
    AppendMenuW(menu, MF_STRING | (slot.numbers ? MF_CHECKED : 0), 4, Text(L"Numbers on icon", L"Цифры на значке"));
    AppendMenuW(menu, MF_STRING, 1, refreshLabel.get());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, 2, Text(L"Exit", L"Выход"));
    POINT cursor{};
    GetCursorPos(&cursor);
    SetForegroundWindow(window);
    const UINT command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON,
                                       cursor.x, cursor.y, 0, window, nullptr);
    PostMessageW(window, WM_NULL, 0, 0);
    DestroyMenu(menu);
    if (command == 2) { DestroyWindow(window); return; }
    if (command == 1) { Refresh(window); return; }
    if (command == 3) { EditSettings(window, index); return; }
    if (command == 4) {
        if (WriteNumber(SlotKey(slot), L"ShowNumbers", slot.numbers ? 0 : 1)) {
            slot.numbers = !slot.numbers;
            RepaintTrayIcon(window, index);
        }
        return;
    }
    if (command == 10 || (command >= 100 && command - 100 < menuDevices.size())) {
        std::wstring choice;
        if (command != 10) {
            const auto& device = menuDevices[command - 100];
            choice = DeviceIdentity(device) == L"usb-keyboard" ? L"usb-keyboard" : device.address;
        }
        if (!WriteString(settingsKey, slot.key, choice)) return;
        if (slot.choice != choice) {
            slot.choice = choice;
            slot.alertDevice.clear();
            slot.alreadyAlerted = false;
            ForgetLastSample(slot);
        }
        ShowStatus(window, index, {}, Text(L"Checking battery", L"проверяю заряд"));
        Refresh(window);
    }
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == taskbarCreated) {
        for (size_t i = 0; i < slots.size(); ++i)
            slots[i].registered = slots[i].inTray && slots[i].icon && Publish(window, i, NIM_ADD);
        return 0;
    }
    switch (message) {
    case WM_SETTINGCHANGE:
    case WM_THEMECHANGED:
    case WM_SYSCOLORCHANGE: {
        UpdateTrayPalette(window);
        if (batteryFlyout) SendMessageW(batteryFlyout, WM_THEMECHANGED, 0, 0);
        return 0;
    }
    case WM_DEVICECHANGE:
        if (wparam == DBT_DEVICEARRIVAL || wparam == DBT_DEVICEREMOVECOMPLETE ||
            wparam == DBT_DEVNODES_CHANGED)
            SetTimer(window, hotplugTimer, 700, nullptr);
        return TRUE;
    case WM_TIMER:
        if (wparam == pollTimer) {
            if (scanning) SendMessageW(window, scanFinished, 0, 0);
            else Refresh(window);
        }
        if (wparam == hotplugTimer) {
            KillTimer(window, hotplugTimer);
            Refresh(window);
        }
        return 0;
    case soundFinished:
        playingSound = false;
        StartNextSound(window);
        return 0;
    case openFlyout:
        if (!batteryFlyout && IsWindowEnabled(window)) {
            const auto anchor = std::find_if(slots.begin(), slots.end(), [](const Slot& slot) { return slot.registered; });
            ShowBatteryFlyout(window, anchor == slots.end() ? 0 : static_cast<size_t>(anchor - slots.begin()));
        }
        return 0;
    case trayMessage:
        if (wparam < 1 || wparam > slots.size() || !IsWindowEnabled(window)) return 0;
        if (lparam == WM_LBUTTONUP) ShowBatteryFlyout(window, wparam - 1);
        if (lparam == WM_RBUTTONUP) {
            if (batteryFlyout) DestroyWindow(batteryFlyout);
            ShowMenu(window, wparam - 1);
        }
        return 0;
    case scanFinished: {
        auto scan = TakeScanResult();
        if (!scan) return 0;
        for (auto& item : scan->devices) {
            if (!item.connected()) continue;
            KeepKnownK75Receiver(item, slots[keyboardSlot].lastSample, listed);
            const auto id = DeviceIdentity(item);
            item.alias = ReadString(deviceNamesKey, id.c_str());
        }
        // The scan may have started before the user changed a slot.
        for (size_t i = 0; i < slots.size(); ++i) {
            auto& slot = slots[i];
            if (slot.choice != scan->choices[i]) continue;
            if (scan->selected[i] >= scan->devices.size()) {
                ShowRememberedStatus(window, i);
                continue;
            }
            const auto& device = scan->devices[scan->selected[i]];
            const auto status = BatteryForDisplay(slot, device);
            ShowStatus(window, i, status, TrayDeviceDetail(device, status));
            if (ShouldAlert(slot, device)) ShowLowCharge(window, i, device);
        }
        listed = std::move(scan->devices);
        scanning = false;
        lastScanAt = GetTickCount64();
        GetLocalTime(&lastScanClock);
        UpdateBatteryFlyout();
        ArmPollTimer(window);
        if (rescan) { rescan = false; Refresh(window); }
        return 0;
    }
    case WM_DESTROY:
        TakeScanResult(true);
        if (batteryFlyout) DestroyWindow(batteryFlyout);
        KillTimer(window, pollTimer);
        KillTimer(window, hotplugTimer);
        for (size_t i = 0; i < slots.size(); ++i) {
            if (slots[i].registered) Publish(window, i, NIM_DELETE);
            if (slots[i].icon) DestroyIcon(slots[i].icon);
        }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR commandLine, int) {
    const bool startup = commandLine && std::wstring_view(commandLine) == L"--startup";
    std::unique_ptr<void, CloseFile> singleton(CreateMutexW(nullptr, FALSE, L"Local\\PeripheralBatteryTray"));
    if (!singleton) return 1;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (!startup) {
            if (const auto running = FindWindowW(trayWindowClass, nullptr)) PostMessageW(running, openFlyout, 0, 0);
        }
        return 0;
    }
    SetProcessDPIAware();
    LoadSettings();
    trayPalette = CurrentTrayPalette();
    taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    WNDCLASSW window{};
    window.hInstance = instance;
    window.lpfnWndProc = WindowProc;
    window.lpszClassName = trayWindowClass;
    window.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(appIconResource));
    if (!RegisterClassW(&window)) return 1;
    const auto handle = CreateWindowExW(WS_EX_TOOLWINDOW, trayWindowClass, appName,
        WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, instance, nullptr);
    if (!handle) return 1;
    scanMailbox.owner = handle;
    DEV_BROADCAST_DEVICEINTERFACE_W filter{};
    filter.dbcc_size = sizeof(filter);
    filter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
    HidD_GetHidGuid(&filter.dbcc_classguid);
    const auto subscription = RegisterDeviceNotificationW(handle, &filter, DEVICE_NOTIFY_WINDOW_HANDLE);
    filter.dbcc_classguid = leInterface;
    const auto leSubscription = RegisterDeviceNotificationW(handle, &filter, DEVICE_NOTIFY_WINDOW_HANDLE);
    for (size_t i = 0; i < slots.size(); ++i) {
        slots[i].lastSample = ReadLastSample(slots[i]);
        ShowRememberedStatus(handle, i);
    }
    ArmPollTimer(handle);
    Refresh(handle);
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (batteryFlyout && IsDialogMessageW(batteryFlyout, &message)) continue;
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    if (subscription) UnregisterDeviceNotification(subscription);
    if (leSubscription) UnregisterDeviceNotification(leSubscription);
    return 0;
}



