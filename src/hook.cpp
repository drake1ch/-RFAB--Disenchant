#include "hook.h"

#include "log.h"

#include "RE/E/EnchantConstructMenu.h"
#include "RE/C/CraftingMenu.h"
#include "RE/E/ExtraDataList.h"
#include "RE/E/ExtraEnchantment.h"
#include "RE/E/ExtraUniqueID.h"
#include "RE/I/InterfaceStrings.h"
#include "RE/I/IMessageBoxCallback.h"
#include "RE/B/BSInputDeviceManager.h"
#include "RE/B/ButtonEvent.h"
#include "RE/I/InputEvent.h"
#include "RE/M/Misc.h"
#include "RE/M/MessageBoxData.h"
#include "RE/M/MessageBoxMenu.h"
#include "RE/RTTI.h"
#include "RE/U/UI.h"
#include "RE/U/UIMessageQueue.h"
#include "RE/U/UserEvents.h"

#include <atomic>
#include <cstring>
#include <mutex>
#include <optional>
#include <string_view>
#include <unordered_set>
#include <vector>
#include <Windows.h>

namespace RFAB::Disenchant
{
    namespace
    {
        constexpr std::uint32_t kSerializationRecordType = 'MARK';
        constexpr std::uint32_t kSerializationVersion = 2;

        enum class MarkSignature : std::uint64_t
        {
        };

        struct MarkSignatureHash
        {
            [[nodiscard]] std::size_t operator()(MarkSignature a_sig) const noexcept
            {
                return std::hash<std::uint64_t>{}(static_cast<std::uint64_t>(a_sig));
            }
        };

        std::mutex g_markedItemsLock;
        std::unordered_set<std::uint64_t> g_markedItems;
        std::unordered_set<MarkSignature, MarkSignatureHash> g_markedSignatures;
        std::atomic<std::uint32_t> g_forceHideMessageBoxUntilMs{ 0 };
        std::atomic<std::uint32_t> g_allowNoDataMessageBoxUntilMs{ 0 };
        std::atomic_bool g_messageBoxShowingOurConfirm{ false };
        std::atomic<std::uint32_t> g_suppressEnchantInputUntilMs{ 0 };
        std::atomic<std::uint32_t> g_suppressConfirmUntilMs{ 0 };
        std::atomic<std::uint32_t> g_nextConfirmAllowedMs{ 0 };
        constexpr std::uint32_t kRemoveHotkeyDIK = 0x13;
        constexpr std::uint32_t kConfirmDebounceMs = 600;
        constexpr auto* kRemoveSuccessSound = "UIEnchantingItemDestroy";
        constexpr auto* kRemoveSuccessNotification =
            "\xD0\x97\xD0\xB0\xD1\x87\xD0\xB0\xD1\x80\xD0\xBE\xD0\xB2\xD0\xB0\xD0\xBD\xD0\xB8\xD0\xB5 "
            "\xD1\x83\xD1\x81\xD0\xBF\xD0\xB5\xD1\x88\xD0\xBD\xD0\xBE "
            "\xD1\x81\xD0\xBD\xD1\x8F\xD1\x82\xD0\xBE";
        constexpr auto* kRemoveConfirmText =
            "\xD0\x92\xD1\x8B \xD1\x82\xD0\xBE\xD1\x87\xD0\xBD\xD0\xBE \xD1\x85\xD0\xBE\xD1\x82\xD0\xB8\xD1\x82\xD0\xB5 \xD0\xBE\xD1\x87\xD0\xB8\xD1\x81\xD1\x82\xD0\xB8\xD1\x82\xD1\x8C \xD0\xB7\xD0\xB0\xD1\x87\xD0\xB0\xD1\x80\xD0\xBE\xD0\xB2\xD0\xB0\xD0\xBD\xD0\xB8\xD0\xB5?";
        constexpr auto* kRemoveConfirmYes = "\xD0\x94\xD0\xB0";
        constexpr auto* kRemoveConfirmNo = "\xD0\x9D\xD0\xB5\xD1\x82";

        struct RemoveConfirmationRequest
        {
            std::optional<std::uint64_t> key;
            std::optional<MarkSignature> signature;
        };

        std::mutex g_removeConfirmLock;
        std::atomic_bool g_removeConfirmOpen{ false };
        std::atomic_bool g_removeConfirmQueued{ false };
        RemoveConfirmationRequest g_removeConfirmRequest;

        using ProcessUserEvent_t = bool(RE::CraftingSubMenus::EnchantConstructMenu*, RE::BSFixedString*);
        REL::Relocation<ProcessUserEvent_t> g_processUserEventOriginal;

        [[nodiscard]] bool IsMarked(std::uint64_t a_key);
        [[nodiscard]] bool IsMarked(const MarkSignature& a_signature);
        [[nodiscard]] bool IsEntryMarked(
            RE::InventoryEntryData* a_entry,
            std::optional<std::uint64_t>* a_markedKey = nullptr,
            std::optional<MarkSignature>* a_markedSignature = nullptr);
        [[nodiscard]] RE::InventoryEntryData* ResolveDisenchantSelection(RE::CraftingSubMenus::EnchantConstructMenu* a_menu);
        [[nodiscard]] RE::CraftingSubMenus::EnchantConstructMenu::ItemChangeEntry* GetHighlightedItemEntry(
            RE::CraftingSubMenus::EnchantConstructMenu* a_menu);
        [[nodiscard]] bool ShouldSuppressVanillaDisenchantPrompt(RE::CraftingSubMenus::EnchantConstructMenu* a_menu);
        [[nodiscard]] bool EntryHasAnyEnchantment(RE::InventoryEntryData* a_entry);
        [[nodiscard]] RE::EnchantmentItem* GetEntryExtraEnchantment(RE::InventoryEntryData* a_entry);
        [[nodiscard]] bool EntryHasExtraEnchantment(RE::InventoryEntryData* a_entry);
        [[nodiscard]] std::optional<std::uint64_t> GetAnyEntryKey(RE::InventoryEntryData* a_entry);
        [[nodiscard]] bool EntryHasKey(RE::InventoryEntryData* a_entry, std::uint64_t a_key);
        void ForceEnableMarkedDisenchantRows(RE::CraftingSubMenus::EnchantConstructMenu* a_menu);
        void DisableStaleDisenchantRows(RE::CraftingSubMenus::EnchantConstructMenu* a_menu);
        void QueueDisenchantPostRemoveRefresh();
        void ArmForceHideNextMessageBox(std::uint32_t a_durationMs);
        void ArmAllowNoDataMessageBox(std::uint32_t a_durationMs);
        [[nodiscard]] bool ShouldForceHideMessageBoxNow();
        [[nodiscard]] bool ShouldAllowNoDataMessageBoxNow();
        void ArmSuppressEnchantInput(std::uint32_t a_durationMs);
        [[nodiscard]] bool ShouldSuppressEnchantInputNow();
        void ArmSuppressConfirm(std::uint32_t a_durationMs);
        [[nodiscard]] bool ShouldSuppressConfirmNow();
        void ShowRemoveConfirmation(
            RE::CraftingSubMenus::EnchantConstructMenu* a_menu,
            const std::optional<std::uint64_t>& a_key,
            const std::optional<MarkSignature>& a_signature);
        [[nodiscard]] RE::CraftingSubMenus::EnchantConstructMenu* GetActiveEnchantConstructMenu();

        [[nodiscard]] bool EntryHasAnyEnchantment(RE::InventoryEntryData* a_entry)
        {
            if (!a_entry) {
                return false;
            }

            return a_entry->IsEnchanted();
        }

        [[nodiscard]] RE::EnchantmentItem* GetEntryExtraEnchantment(RE::InventoryEntryData* a_entry)
        {
            if (!a_entry || !a_entry->extraLists) {
                return nullptr;
            }

            for (auto* extraList : *a_entry->extraLists) {
                if (!extraList) {
                    continue;
                }

                if (const auto* extra = extraList->GetByType<RE::ExtraEnchantment>(); extra && extra->enchantment) {
                    return extra->enchantment;
                }
            }

            return nullptr;
        }

        [[nodiscard]] bool EntryHasExtraEnchantment(RE::InventoryEntryData* a_entry)
        {
            return GetEntryExtraEnchantment(a_entry) != nullptr;
        }

        class RemoveHotkeySink final : public RE::BSTEventSink<RE::InputEvent*>
        {
        public:
            RE::BSEventNotifyControl ProcessEvent(
                RE::InputEvent* const* a_events,
                RE::BSTEventSource<RE::InputEvent*>*) override
            {
                if (!a_events || !*a_events) {
                    return RE::BSEventNotifyControl::kContinue;
                }

                auto* menu = GetActiveEnchantConstructMenu();
                if (!menu || menu->currentCategory != RE::CraftingSubMenus::EnchantConstructMenu::Category::Disenchant) {
                    return RE::BSEventNotifyControl::kContinue;
                }

                {
                    std::scoped_lock lk(g_removeConfirmLock);
                    if (g_removeConfirmOpen.load(std::memory_order_acquire) ||
                        g_removeConfirmQueued.load(std::memory_order_acquire)) {
                        return RE::BSEventNotifyControl::kContinue;
                    }
                }

                for (auto* e = *a_events; e; e = e->next) {
                    if (e->eventType != RE::INPUT_EVENT_TYPE::kButton) {
                        continue;
                    }

                    auto* btn = static_cast<RE::ButtonEvent*>(e);
                    if (!btn || !btn->IsDown()) {
                        continue;
                    }

                    if (btn->GetIDCode() != kRemoveHotkeyDIK) {
                        continue;
                    }

                    auto* entry = ResolveDisenchantSelection(menu);
                    std::optional<std::uint64_t> key;
                    std::optional<MarkSignature> sig;
                    const auto marked = entry && IsEntryMarked(entry, &key, &sig);
                    if (!marked || (!key && !sig)) {
                        return RE::BSEventNotifyControl::kContinue;
                    }

                    ShowRemoveConfirmation(menu, key, sig);
                    return RE::BSEventNotifyControl::kStop;
                }

                return RE::BSEventNotifyControl::kContinue;
            }
        };

        RemoveHotkeySink g_removeHotkeySink;

        [[nodiscard]] std::uint64_t MakeMarkKey(const RE::ExtraUniqueID& a_uniqueID)
        {
            return (static_cast<std::uint64_t>(a_uniqueID.baseID) << 16u) | static_cast<std::uint64_t>(a_uniqueID.uniqueID);
        }

        [[nodiscard]] constexpr MarkSignature MakeMarkSignature(std::uint32_t a_objectFormID, std::uint32_t a_enchantmentFormID) noexcept
        {
            const auto raw = (static_cast<std::uint64_t>(a_objectFormID) << 32u) | static_cast<std::uint64_t>(a_enchantmentFormID);
            return static_cast<MarkSignature>(raw);
        }

        [[nodiscard]] std::optional<MarkSignature> GetEntryMarkSignature(RE::InventoryEntryData* a_entry)
        {
            if (!a_entry) {
                return std::nullopt;
            }

            const auto* object = a_entry->object;
            const auto* enchantment = a_entry->GetEnchantment();
            if (!enchantment) {
                enchantment = GetEntryExtraEnchantment(a_entry);
            }
            if (!object || !enchantment) {
                return std::nullopt;
            }

            return MakeMarkSignature(object->GetFormID(), enchantment->GetFormID());
        }

        [[nodiscard]] std::optional<std::uint64_t> FindMarkedKeyInEntry(RE::InventoryEntryData* a_entry)
        {
            if (!a_entry || !a_entry->extraLists) {
                return std::nullopt;
            }

            for (auto* extraList : *a_entry->extraLists) {
                if (!extraList) {
                    continue;
                }

                const auto* uniqueID = extraList->GetByType<RE::ExtraUniqueID>();
                if (!uniqueID) {
                    continue;
                }

                const auto key = MakeMarkKey(*uniqueID);
                if (IsMarked(key)) {
                    return key;
                }
            }

            return std::nullopt;
        }

        [[nodiscard]] std::optional<std::uint64_t> GetAnyEntryKey(RE::InventoryEntryData* a_entry)
        {
            if (!a_entry || !a_entry->extraLists) {
                return std::nullopt;
            }

            for (auto* extraList : *a_entry->extraLists) {
                if (!extraList) {
                    continue;
                }

                if (const auto* uniqueID = extraList->GetByType<RE::ExtraUniqueID>()) {
                    return MakeMarkKey(*uniqueID);
                }
            }

            return std::nullopt;
        }

        [[nodiscard]] bool EntryHasKey(RE::InventoryEntryData* a_entry, std::uint64_t a_key)
        {
            if (!a_entry || !a_entry->extraLists) {
                return false;
            }

            for (auto* extraList : *a_entry->extraLists) {
                if (!extraList) {
                    continue;
                }

                if (const auto* uniqueID = extraList->GetByType<RE::ExtraUniqueID>()) {
                    if (MakeMarkKey(*uniqueID) == a_key) {
                        return true;
                    }
                }
            }

            return false;
        }

        [[nodiscard]] std::optional<std::uint64_t> GetSelectedEntryMarkKey(RE::CraftingSubMenus::EnchantConstructMenu* a_menu)
        {
            if (!a_menu || !a_menu->selected.item) {
                return std::nullopt;
            }

            return GetAnyEntryKey(a_menu->selected.item->data);
        }

        [[nodiscard]] std::optional<MarkSignature> GetSelectedEntryMarkSignature(RE::CraftingSubMenus::EnchantConstructMenu* a_menu)
        {
            if (!a_menu || !a_menu->selected.item) {
                return std::nullopt;
            }

            return GetEntryMarkSignature(a_menu->selected.item->data);
        }

        [[nodiscard]] RE::CraftingSubMenus::EnchantConstructMenu::ItemChangeEntry* GetHighlightedItemEntry(
            RE::CraftingSubMenus::EnchantConstructMenu* a_menu)
        {
            if (!a_menu || a_menu->highlightIndex >= a_menu->listEntries.size()) {
                return nullptr;
            }

            return skyrim_cast<RE::CraftingSubMenus::EnchantConstructMenu::ItemChangeEntry*>(
                a_menu->listEntries[a_menu->highlightIndex].get());
        }

        [[nodiscard]] bool ShouldSuppressVanillaDisenchantPrompt(
            RE::CraftingSubMenus::EnchantConstructMenu* a_menu)
        {
            if (!a_menu || a_menu->currentCategory != RE::CraftingSubMenus::EnchantConstructMenu::Category::Disenchant) {
                return false;
            }

            auto* resolvedEntryData = ResolveDisenchantSelection(a_menu);
            auto* selectedItemEntry = a_menu->selected.item ? a_menu->selected.item.get() : nullptr;
            auto* highlightedItemEntry = GetHighlightedItemEntry(a_menu);

            auto* selectedEntryData = (selectedItemEntry && selectedItemEntry->data) ? selectedItemEntry->data : nullptr;
            auto* highlightedEntryData = (highlightedItemEntry && highlightedItemEntry->data) ? highlightedItemEntry->data : nullptr;

            const auto selectedMarked = selectedEntryData && IsEntryMarked(selectedEntryData);
            const auto highlightedMarked = highlightedEntryData && IsEntryMarked(highlightedEntryData);
            const auto resolvedMarked = resolvedEntryData && IsEntryMarked(resolvedEntryData);
            if (selectedMarked || highlightedMarked || resolvedMarked) {
                return true;
            }

            const auto selectedHasEnchant = selectedEntryData && EntryHasAnyEnchantment(selectedEntryData);
            const auto highlightedHasEnchant = highlightedEntryData && EntryHasAnyEnchantment(highlightedEntryData);
            const auto resolvedHasEnchant = resolvedEntryData && EntryHasAnyEnchantment(resolvedEntryData);

            const auto hasVisibleTarget = (highlightedEntryData != nullptr) || (selectedEntryData != nullptr) || (resolvedEntryData != nullptr);
            const auto activeHasEnchant =
                highlightedEntryData ? highlightedHasEnchant :
                (selectedEntryData ? selectedHasEnchant : resolvedHasEnchant);

            if (hasVisibleTarget && !activeHasEnchant) {
                return true;
            }

            return false;
        }

        [[nodiscard]] RE::InventoryEntryData* ResolveDisenchantSelection(RE::CraftingSubMenus::EnchantConstructMenu* a_menu)
        {
            if (!a_menu) {
                return nullptr;
            }

            if (a_menu->selected.item && a_menu->selected.item->data) {
                return a_menu->selected.item->data;
            }

            if (a_menu->highlightIndex < a_menu->listEntries.size()) {
                auto* highlightedEntry = skyrim_cast<RE::CraftingSubMenus::EnchantConstructMenu::ItemChangeEntry*>(a_menu->listEntries[a_menu->highlightIndex].get());
                if (highlightedEntry && highlightedEntry->data) {
                    return highlightedEntry->data;
                }
            }

            for (auto& listEntry : a_menu->listEntries) {
                auto* itemEntry = skyrim_cast<RE::CraftingSubMenus::EnchantConstructMenu::ItemChangeEntry*>(listEntry.get());
                if (itemEntry && itemEntry->selected && itemEntry->data) {
                    return itemEntry->data;
                }
            }

            for (auto& listEntry : a_menu->listEntries) {
                auto* itemEntry = skyrim_cast<RE::CraftingSubMenus::EnchantConstructMenu::ItemChangeEntry*>(listEntry.get());
                if (itemEntry && itemEntry->data) {
                    return itemEntry->data;
                }
            }

            return nullptr;
        }

        void MarkItem(std::uint64_t a_key)
        {
            std::scoped_lock lk(g_markedItemsLock);
            g_markedItems.insert(a_key);
        }

        void MarkItem(const MarkSignature& a_signature)
        {
            std::scoped_lock lk(g_markedItemsLock);
            g_markedSignatures.insert(a_signature);
        }

        void UnmarkItem(std::uint64_t a_key)
        {
            std::scoped_lock lk(g_markedItemsLock);
            g_markedItems.erase(a_key);
        }

        void UnmarkItem(const MarkSignature& a_signature)
        {
            std::scoped_lock lk(g_markedItemsLock);
            g_markedSignatures.erase(a_signature);
        }

        [[nodiscard]] bool IsMarked(std::uint64_t a_key)
        {
            std::scoped_lock lk(g_markedItemsLock);
            return g_markedItems.contains(a_key);
        }

        [[nodiscard]] bool IsMarked(const MarkSignature& a_signature)
        {
            std::scoped_lock lk(g_markedItemsLock);
            return g_markedSignatures.contains(a_signature);
        }

        [[nodiscard]] bool IsEntryMarked(
            RE::InventoryEntryData* a_entry,
            std::optional<std::uint64_t>* a_markedKey,
            std::optional<MarkSignature>* a_markedSignature)
        {
            if (a_markedKey) {
                *a_markedKey = std::nullopt;
            }

            if (a_markedSignature) {
                *a_markedSignature = std::nullopt;
            }

            if (!a_entry) {
                return false;
            }

            const auto key = FindMarkedKeyInEntry(a_entry);
            if (key) {
                if (a_markedKey) {
                    *a_markedKey = key;
                }
                return true;
            }

            const auto signature = GetEntryMarkSignature(a_entry);
            if (signature && IsMarked(*signature)) {
                if (a_markedSignature) {
                    *a_markedSignature = signature;
                }
                return true;
            }

            if (EntryHasExtraEnchantment(a_entry)) {
                if (a_markedKey) {
                    *a_markedKey = GetAnyEntryKey(a_entry);
                }
                if (a_markedSignature) {
                    *a_markedSignature = GetEntryMarkSignature(a_entry);
                }
                return true;
            }

            return false;
        }

        [[nodiscard]] bool RemoveEnchantmentFromEntry(RE::InventoryEntryData* a_entry)
        {
            if (!a_entry || !a_entry->extraLists) {
                return false;
            }

            bool changed = false;

            for (auto* extraList : *a_entry->extraLists) {
                if (!extraList) {
                    continue;
                }

                const auto removedEnchant = extraList->RemoveByType(RE::ExtraDataType::kEnchantment);
                const auto removedCharge = extraList->RemoveByType(RE::ExtraDataType::kCharge);
                changed = changed || removedEnchant || removedCharge;
                if (removedEnchant || removedCharge) {
                    auto* player = RE::PlayerCharacter::GetSingleton();
                    if (player) {
                        if (auto* invChanges = player->GetInventoryChanges()) {
                            invChanges->changed = true;
                        }
                    }
                }
            }

            return changed;
        }

        [[nodiscard]] bool ItemHasExtraEnchantment(std::uint64_t a_key)
        {
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player) {
                return false;
            }

            const auto inventory = player->GetInventory();
            for (const auto& [obj, invPair] : inventory) {
                (void)obj;
                const auto& [count, entry] = invPair;
                (void)count;
                if (!entry) {
                    continue;
                }

                if (!EntryHasKey(entry.get(), a_key) || !entry->extraLists) {
                    continue;
                }

                for (auto* extraList : *entry->extraLists) {
                    if (extraList && extraList->HasType<RE::ExtraEnchantment>()) {
                        return true;
                    }
                }
            }

            return false;
        }

        [[nodiscard]] bool ItemHasExtraEnchantment(const MarkSignature& a_signature)
        {
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player) {
                return false;
            }

            const auto inventory = player->GetInventory();
            for (const auto& [obj, invPair] : inventory) {
                (void)obj;
                const auto& [count, entry] = invPair;
                (void)count;
                if (!entry) {
                    continue;
                }

                const auto signature = GetEntryMarkSignature(entry.get());
                if (!signature || *signature != a_signature) {
                    continue;
                }

                if (entry->GetEnchantment()) {
                    return true;
                }

                if (!entry->extraLists) {
                    continue;
                }

                for (auto* extraList : *entry->extraLists) {
                    if (extraList && extraList->HasType<RE::ExtraEnchantment>()) {
                        return true;
                    }
                }
            }

            return false;
        }

        [[nodiscard]] bool ItemExistsInPlayerInventory(std::uint64_t a_key)
        {
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player) {
                return false;
            }

            const auto inventory = player->GetInventory();
            for (const auto& [obj, invPair] : inventory) {
                (void)obj;
                const auto& [count, entry] = invPair;
                (void)count;
                if (!entry) {
                    continue;
                }

                if (EntryHasKey(entry.get(), a_key)) {
                    return true;
                }
            }

            return false;
        }

        [[nodiscard]] bool ItemExistsInPlayerInventory(const MarkSignature& a_signature)
        {
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player) {
                return false;
            }

            const auto inventory = player->GetInventory();
            for (const auto& [obj, invPair] : inventory) {
                (void)obj;
                const auto& [count, entry] = invPair;
                (void)count;
                if (!entry) {
                    continue;
                }

                const auto signature = GetEntryMarkSignature(entry.get());
                if (signature && *signature == a_signature) {
                    return true;
                }
            }

            return false;
        }

        [[nodiscard]] bool RemoveEnchantmentFromMarkedInstance(RE::InventoryEntryData* a_entry, std::uint64_t a_key)
        {
            if (!a_entry || !a_entry->extraLists) {
                return false;
            }

            for (auto* extraList : *a_entry->extraLists) {
                if (!extraList) {
                    continue;
                }

                const auto* uniqueID = extraList->GetByType<RE::ExtraUniqueID>();
                if (!uniqueID || MakeMarkKey(*uniqueID) != a_key) {
                    continue;
                }

                const auto removedEnchant = extraList->RemoveByType(RE::ExtraDataType::kEnchantment);
                const auto removedCharge = extraList->RemoveByType(RE::ExtraDataType::kCharge);
                if (removedEnchant || removedCharge) {
                    auto* player = RE::PlayerCharacter::GetSingleton();
                    if (player) {
                        if (auto* invChanges = player->GetInventoryChanges()) {
                            invChanges->changed = true;
                        }
                    }
                }
                return removedEnchant || removedCharge;
            }

            return false;
        }

        [[nodiscard]] RE::InventoryEntryData* FindMarkedEntryInMenu(
            RE::CraftingSubMenus::EnchantConstructMenu* a_menu,
            const std::optional<std::uint64_t>& a_key,
            const std::optional<MarkSignature>& a_signature)
        {
            if (!a_menu) {
                return nullptr;
            }

            if (a_key) {
                for (auto& listEntry : a_menu->listEntries) {
                    auto* itemEntry = skyrim_cast<RE::CraftingSubMenus::EnchantConstructMenu::ItemChangeEntry*>(listEntry.get());
                    if (!itemEntry || !itemEntry->data) {
                        continue;
                    }

                    if (EntryHasKey(itemEntry->data, *a_key)) {
                        return itemEntry->data;
                    }
                }
            }

            if (a_signature) {
                for (auto& listEntry : a_menu->listEntries) {
                    auto* itemEntry = skyrim_cast<RE::CraftingSubMenus::EnchantConstructMenu::ItemChangeEntry*>(listEntry.get());
                    if (!itemEntry || !itemEntry->data) {
                        continue;
                    }

                    const auto signature = GetEntryMarkSignature(itemEntry->data);
                    if (signature && *signature == *a_signature) {
                        return itemEntry->data;
                    }
                }
            }

            return ResolveDisenchantSelection(a_menu);
        }

        [[nodiscard]] RE::InventoryEntryData* FindMarkedEntryInPlayerInventory(
            const std::optional<std::uint64_t>& a_key,
            const std::optional<MarkSignature>& a_signature)
        {
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player) {
                return nullptr;
            }

            const auto inventory = player->GetInventory();
            for (const auto& [obj, invPair] : inventory) {
                (void)obj;
                const auto& [count, entry] = invPair;
                (void)count;
                if (!entry) {
                    continue;
                }

                if (a_key) {
                    if (EntryHasKey(entry.get(), *a_key)) {
                        return entry.get();
                    }
                }

                if (a_signature) {
                    const auto signature = GetEntryMarkSignature(entry.get());
                    if (signature && *signature == *a_signature) {
                        return entry.get();
                    }
                }
            }

            return nullptr;
        }

        [[nodiscard]] RE::CraftingSubMenus::EnchantConstructMenu* GetActiveEnchantConstructMenu()
        {
            auto* ui = RE::UI::GetSingleton();
            if (!ui) {
                return nullptr;
            }

            auto craftingMenu = ui->GetMenu<RE::CraftingMenu>();
            if (!craftingMenu) {
                return nullptr;
            }

            auto* subMenu = craftingMenu->GetCraftingSubMenu();
            return skyrim_cast<RE::CraftingSubMenus::EnchantConstructMenu*>(subMenu);
        }

        [[nodiscard]] bool RemoveMarkedItem(
            RE::CraftingSubMenus::EnchantConstructMenu* a_menu,
            const std::optional<std::uint64_t>& a_preferredKey,
            const std::optional<MarkSignature>& a_preferredSignature)
        {
            auto key = a_preferredKey;
            auto signature = a_preferredSignature;
            auto* entry = FindMarkedEntryInMenu(a_menu, key, signature);
            if (!entry) {
                entry = FindMarkedEntryInPlayerInventory(key, signature);
            }
            if (!entry) {
                return false;
            }

            if (!key && !signature) {
                if (!IsEntryMarked(entry, &key, &signature)) {
                    return false;
                }
            }

            bool removed = false;
            if (key) {
                removed = RemoveEnchantmentFromMarkedInstance(entry, *key);
            }

            if (!removed && signature) {
                removed = RemoveEnchantmentFromEntry(entry);
            }

            if (!removed) {
                return false;
            }

            if (key) {
                UnmarkItem(*key);
            }

            if (signature) {
                UnmarkItem(*signature);
            }

            if (a_menu) {
                a_menu->UpdateConstructibleList();
                a_menu->UpdateInterface();
                DisableStaleDisenchantRows(a_menu);
                QueueDisenchantPostRemoveRefresh();
            }
            return true;
        }

        class RemoveConfirmCallback final : public RE::IMessageBoxCallback
        {
        public:
            RemoveConfirmCallback()
            {
                unk0C = 0;
            }

            void Run(Message a_msg) override
            {
                RemoveConfirmationRequest request;
                {
                    std::scoped_lock lk(g_removeConfirmLock);
                    g_removeConfirmOpen.store(false, std::memory_order_release);
                    request = g_removeConfirmRequest;
                    g_removeConfirmRequest = {};
                }

                g_messageBoxShowingOurConfirm.store(false, std::memory_order_release);

                if (a_msg != Message::kUnk0 || (!request.key && !request.signature)) {
                    return;
                }

                g_allowNoDataMessageBoxUntilMs.store(0, std::memory_order_release);
                if (auto* queue = RE::UIMessageQueue::GetSingleton()) {
                    queue->AddMessage(RE::MessageBoxMenu::MENU_NAME, RE::UI_MESSAGE_TYPE::kForceHide, nullptr);
                }
                ArmForceHideNextMessageBox(3000);
                ArmSuppressEnchantInput(1500);

                auto* menu = GetActiveEnchantConstructMenu();
                const auto removed = RemoveMarkedItem(menu, request.key, request.signature);
                if (removed) {
                    RE::PlaySound(kRemoveSuccessSound);
                    RE::DebugNotification(kRemoveSuccessNotification);
                }
            }
        };

        void ShowRemoveConfirmation(
            RE::CraftingSubMenus::EnchantConstructMenu* a_menu,
            const std::optional<std::uint64_t>& a_key,
            const std::optional<MarkSignature>& a_signature)
        {
            if (!a_menu || (!a_key && !a_signature)) {
                return;
            }

            {
                std::scoped_lock lk(g_removeConfirmLock);
                if (g_removeConfirmOpen.load(std::memory_order_acquire) ||
                    g_removeConfirmQueued.load(std::memory_order_acquire)) {
                    return;
                }

                const auto now = RE::GetDurationOfApplicationRunTime();
                const auto allowAt = g_nextConfirmAllowedMs.load(std::memory_order_acquire);
                if (allowAt != 0 && now < allowAt) {
                    return;
                }

                g_removeConfirmOpen.store(true, std::memory_order_release);
                g_removeConfirmQueued.store(true, std::memory_order_release);
                g_removeConfirmRequest = { a_key, a_signature };
                g_nextConfirmAllowedMs.store(now + kConfirmDebounceMs, std::memory_order_release);
            }

            auto* task = SKSE::GetTaskInterface();
            if (!task) {
                std::scoped_lock lk(g_removeConfirmLock);
                g_removeConfirmOpen.store(false, std::memory_order_release);
                g_removeConfirmQueued.store(false, std::memory_order_release);
                g_removeConfirmRequest = {};
                return;
            }

            task->AddUITask([]() {
                bool shouldShow = false;
                {
                    std::scoped_lock lk(g_removeConfirmLock);
                    shouldShow = g_removeConfirmOpen.load(std::memory_order_acquire) &&
                                 (g_removeConfirmRequest.key.has_value() || g_removeConfirmRequest.signature.has_value());
                }

                if (!shouldShow) {
                    std::scoped_lock lk(g_removeConfirmLock);
                    g_removeConfirmOpen.store(false, std::memory_order_release);
                    g_removeConfirmQueued.store(false, std::memory_order_release);
                    g_removeConfirmRequest = {};
                    return;
                }

                auto* strings = RE::InterfaceStrings::GetSingleton();
                auto* factory = RE::MessageDataFactoryManager::GetSingleton();
                if (!strings || !factory) {
                    std::scoped_lock lk(g_removeConfirmLock);
                    g_removeConfirmOpen.store(false, std::memory_order_release);
                    g_removeConfirmQueued.store(false, std::memory_order_release);
                    g_removeConfirmRequest = {};
                    return;
                }

                const auto* creator = factory->GetCreator<RE::MessageBoxData>(strings->messageBoxData);
                if (!creator) {
                    std::scoped_lock lk(g_removeConfirmLock);
                    g_removeConfirmOpen.store(false, std::memory_order_release);
                    g_removeConfirmQueued.store(false, std::memory_order_release);
                    g_removeConfirmRequest = {};
                    return;
                }

                auto* data = creator->Create();
                if (!data) {
                    std::scoped_lock lk(g_removeConfirmLock);
                    g_removeConfirmOpen.store(false, std::memory_order_release);
                    g_removeConfirmQueued.store(false, std::memory_order_release);
                    g_removeConfirmRequest = {};
                    return;
                }

                data->bodyText = kRemoveConfirmText;
                data->buttonText.clear();
                data->buttonText.push_back(kRemoveConfirmYes);
                data->buttonText.push_back(kRemoveConfirmNo);
                data->type = 0;
                data->cancelOptionIndex = 1;
                data->callback.reset(new RemoveConfirmCallback());
                data->menuDepth = 10;
                data->optionIndexOffset = 0;
                data->useHtml = false;
                data->verticalButtons = false;
                data->isCancellable = true;

                ArmAllowNoDataMessageBox(250);
                g_messageBoxShowingOurConfirm.store(true, std::memory_order_release);
                data->QueueMessage();

                {
                    std::scoped_lock lk(g_removeConfirmLock);
                    g_removeConfirmQueued.store(false, std::memory_order_release);
                }
            });
        }

        [[nodiscard]] bool IsLearnControl(const RE::BSFixedString* a_control)
        {
            if (!a_control || !a_control->data()) {
                return false;
            }

            const auto* events = RE::UserEvents::GetSingleton();
            if (events && (*a_control == events->accept || *a_control == events->click || *a_control == events->yButton ||
                           *a_control == events->activate || *a_control == events->equip)) {
                return true;
            }

            const std::string_view controlName = a_control->data();
            return controlName == "Accept" || controlName == "accept" || controlName == "Click" ||
                   controlName == "click" || controlName == "YButton" || controlName == "yButton" ||
                   controlName == "Activate" || controlName == "activate" || controlName == "Equip" ||
                   controlName == "equip";
        }

        void QueueDisenchantPostRemoveRefresh()
        {
            auto* task = SKSE::GetTaskInterface();
            if (!task) {
                SKSE::log::error("QueueDisenchantPostRemoveRefresh: task interface is null");
                return;
            }

            task->AddUITask([]() {
                if (auto* queue = RE::UIMessageQueue::GetSingleton()) {
                    queue->AddMessage(RE::CraftingMenu::MENU_NAME, RE::UI_MESSAGE_TYPE::kHide, nullptr);
                    queue->AddMessage(RE::CraftingMenu::MENU_NAME, RE::UI_MESSAGE_TYPE::kShow, nullptr);
                    queue->AddMessage(RE::CraftingMenu::MENU_NAME, RE::UI_MESSAGE_TYPE::kUpdate, nullptr);
                }

                auto* menu = GetActiveEnchantConstructMenu();
                if (!menu) {
                    return;
                }

                menu->UpdateConstructibleList();
                DisableStaleDisenchantRows(menu);
                ForceEnableMarkedDisenchantRows(menu);
                menu->UpdateInterface();
            });
        }

        void ArmForceHideNextMessageBox(std::uint32_t a_durationMs)
        {
            const auto now = RE::GetDurationOfApplicationRunTime();
            const auto until = now + a_durationMs;
            g_forceHideMessageBoxUntilMs.store(until, std::memory_order_release);
        }

        void ArmAllowNoDataMessageBox(std::uint32_t a_durationMs)
        {
            const auto now = RE::GetDurationOfApplicationRunTime();
            g_allowNoDataMessageBoxUntilMs.store(now + a_durationMs, std::memory_order_release);
        }

        [[nodiscard]] bool ShouldForceHideMessageBoxNow()
        {
            const auto until = g_forceHideMessageBoxUntilMs.load(std::memory_order_acquire);
            if (until == 0) {
                return false;
            }

            const auto now = RE::GetDurationOfApplicationRunTime();
            if (now > until) {
                g_forceHideMessageBoxUntilMs.store(0, std::memory_order_release);
                return false;
            }

            return true;
        }

        [[nodiscard]] bool ShouldAllowNoDataMessageBoxNow()
        {
            const auto until = g_allowNoDataMessageBoxUntilMs.load(std::memory_order_acquire);
            if (until == 0) {
                return false;
            }

            const auto now = RE::GetDurationOfApplicationRunTime();
            if (now > until) {
                g_allowNoDataMessageBoxUntilMs.store(0, std::memory_order_release);
                return false;
            }

            return true;
        }

        void ArmSuppressEnchantInput(std::uint32_t a_durationMs)
        {
            const auto now = RE::GetDurationOfApplicationRunTime();
            g_suppressEnchantInputUntilMs.store(now + a_durationMs, std::memory_order_release);
        }

        [[nodiscard]] bool ShouldSuppressEnchantInputNow()
        {
            const auto until = g_suppressEnchantInputUntilMs.load(std::memory_order_acquire);
            if (until == 0) {
                return false;
            }

            const auto now = RE::GetDurationOfApplicationRunTime();
            if (now > until) {
                g_suppressEnchantInputUntilMs.store(0, std::memory_order_release);
                return false;
            }

            return true;
        }

        void ArmSuppressConfirm(std::uint32_t a_durationMs)
        {
            const auto now = RE::GetDurationOfApplicationRunTime();
            g_suppressConfirmUntilMs.store(now + a_durationMs, std::memory_order_release);
            g_nextConfirmAllowedMs.store(now + a_durationMs, std::memory_order_release);
        }

        [[nodiscard]] bool ShouldSuppressConfirmNow()
        {
            const auto until = g_suppressConfirmUntilMs.load(std::memory_order_acquire);
            if (until == 0) {
                return false;
            }

            const auto now = RE::GetDurationOfApplicationRunTime();
            if (now > until) {
                g_suppressConfirmUntilMs.store(0, std::memory_order_release);
                return false;
            }

            return true;
        }

        void ForceEnableMarkedDisenchantRows(RE::CraftingSubMenus::EnchantConstructMenu* a_menu)
        {
            if (!a_menu || a_menu->currentCategory != RE::CraftingSubMenus::EnchantConstructMenu::Category::Disenchant) {
                return;
            }

            for (auto& listEntry : a_menu->listEntries) {
                auto* itemEntry = skyrim_cast<RE::CraftingSubMenus::EnchantConstructMenu::ItemChangeEntry*>(listEntry.get());
                if (!itemEntry || !itemEntry->data) {
                    continue;
                }

                if (IsEntryMarked(itemEntry->data)) {
                    itemEntry->enabled = true;
                }
            }
        }

        void DisableStaleDisenchantRows(RE::CraftingSubMenus::EnchantConstructMenu* a_menu)
        {
            if (!a_menu || a_menu->currentCategory != RE::CraftingSubMenus::EnchantConstructMenu::Category::Disenchant) {
                return;
            }

            for (auto& listEntry : a_menu->listEntries) {
                auto* itemEntry = skyrim_cast<RE::CraftingSubMenus::EnchantConstructMenu::ItemChangeEntry*>(listEntry.get());
                if (!itemEntry || !itemEntry->data) {
                    continue;
                }

                if (!EntryHasAnyEnchantment(itemEntry->data)) {
                    itemEntry->enabled = false;
                    itemEntry->selected = false;
                }
            }

            if (a_menu->selected.item && a_menu->selected.item->data && !EntryHasAnyEnchantment(a_menu->selected.item->data)) {
                a_menu->selected.item.reset();
            }

        }

        void SelectDisenchantEntryWithoutAction(
            RE::CraftingSubMenus::EnchantConstructMenu* a_menu,
            RE::CraftingSubMenus::EnchantConstructMenu::ItemChangeEntry* a_target)
        {
            if (!a_menu || !a_target) {
                return;
            }

            std::uint32_t selectedIndex = 0;
            bool found = false;
            for (std::uint32_t i = 0; i < a_menu->listEntries.size(); ++i) {
                auto* itemEntry = skyrim_cast<RE::CraftingSubMenus::EnchantConstructMenu::ItemChangeEntry*>(a_menu->listEntries[i].get());
                if (!itemEntry) {
                    continue;
                }

                const auto isTarget = itemEntry == a_target;
                itemEntry->selected = isTarget;
                if (isTarget) {
                    selectedIndex = i;
                    found = true;
                }
            }

            if (found) {
                a_menu->highlightIndex = selectedIndex;

                a_menu->UpdateInterface();
            }
        }

        struct ItemChangeSetDataHook
        {
            static void SetData_Thunk(
                RE::CraftingSubMenus::EnchantConstructMenu::ItemChangeEntry* a_this,
                RE::GFxValue* a_dataContainer)
            {
                using FilterFlag = RE::CraftingSubMenus::EnchantConstructMenu::FilterFlag;
                const auto inDisenchantList =
                    a_this && a_this->filterFlag.any(FilterFlag::DisenchantWeapon, FilterFlag::DisenchantArmor);
                const auto isMarked = inDisenchantList && a_this && a_this->data && IsEntryMarked(a_this->data);
                const auto isStaleDisenchantRow =
                    inDisenchantList && a_this && a_this->data && !EntryHasAnyEnchantment(a_this->data);

                if (isMarked) {
                    a_this->enabled = true;
                } else if (isStaleDisenchantRow) {
                    a_this->enabled = false;
                }

                SetData_Original(a_this, a_dataContainer);

                if (isMarked && a_dataContainer && a_dataContainer->IsObject()) {
                    a_dataContainer->SetMember("enabled", RE::GFxValue(true));
                    a_dataContainer->SetMember("isEnabled", RE::GFxValue(true));
                    a_dataContainer->SetMember("disabled", RE::GFxValue(false));
                    a_dataContainer->SetMember("known", RE::GFxValue(false));
                    a_dataContainer->SetMember("isKnown", RE::GFxValue(false));
                }
                if (isStaleDisenchantRow && a_dataContainer && a_dataContainer->IsObject()) {
                    a_dataContainer->SetMember("enabled", RE::GFxValue(false));
                    a_dataContainer->SetMember("isEnabled", RE::GFxValue(false));
                    a_dataContainer->SetMember("disabled", RE::GFxValue(true));
                }
            }

            static void Install()
            {
                REL::Relocation<std::uintptr_t> vtbl{ RE::VTABLE_CraftingSubMenus__EnchantConstructMenu__ItemChangeEntry[0] };
                SetData_Original = vtbl.write_vfunc(0x4, SetData_Thunk);
            }

            static inline REL::Relocation<decltype(SetData_Thunk)> SetData_Original;
        };

        struct ItemChangeActivateHook
        {
            static void Activate_Thunk(RE::CraftingSubMenus::EnchantConstructMenu::ItemChangeEntry* a_this)
            {
                auto* menu = GetActiveEnchantConstructMenu();
                const auto inDisenchant =
                    menu && menu->currentCategory == RE::CraftingSubMenus::EnchantConstructMenu::Category::Disenchant;

                if (inDisenchant && ShouldSuppressEnchantInputNow()) {
                    return;
                }
                const auto hasEnchant = a_this && a_this->data && EntryHasAnyEnchantment(a_this->data);
                std::optional<std::uint64_t> markedKey;
                std::optional<MarkSignature> markedSignature;
                const auto isMarked =
                    inDisenchant && a_this && a_this->data && IsEntryMarked(a_this->data, &markedKey, &markedSignature);

                if (inDisenchant && !hasEnchant) {
                    return;
                }

                if (inDisenchant && isMarked) {
                    SelectDisenchantEntryWithoutAction(menu, a_this);
                    ForceEnableMarkedDisenchantRows(menu);
                    return;
                }

                Activate_Original(a_this);
            }

            static void Install()
            {
                REL::Relocation<std::uintptr_t> vtbl{ RE::VTABLE_CraftingSubMenus__EnchantConstructMenu__ItemChangeEntry[0] };
                Activate_Original = vtbl.write_vfunc(0x2, Activate_Thunk);
            }

            static inline REL::Relocation<decltype(Activate_Thunk)> Activate_Original;
        };

        struct MessageBoxMenuProcessMessageHook
        {
            static RE::UI_MESSAGE_RESULTS ProcessMessage_Thunk(RE::MessageBoxMenu* a_this, RE::UIMessage& a_message)
            {
                const auto type = a_message.type.get();
                const auto showLike =
                    type == RE::UI_MESSAGE_TYPE::kShow ||
                    type == RE::UI_MESSAGE_TYPE::kReshow ||
                    type == RE::UI_MESSAGE_TYPE::kUpdate;

                bool allowThis = false;
                if (showLike && a_message.data) {
                    static const std::uintptr_t kRemoveConfirmCallbackVTable = []() {
                        RemoveConfirmCallback cb;
                        return *reinterpret_cast<std::uintptr_t*>(&cb);
                    }();

                    auto* data = static_cast<RE::MessageBoxData*>(a_message.data);
                    const auto* body = data ? data->bodyText.c_str() : nullptr;
                    const auto callback = data ? data->callback.get() : nullptr;
                    const auto callbackVTable = callback ? *reinterpret_cast<std::uintptr_t*>(callback) : 0;
                    allowThis =
                        (callbackVTable != 0 && callbackVTable == kRemoveConfirmCallbackVTable) ||
                        (body && std::strcmp(body, kRemoveConfirmText) == 0);
                    if (allowThis) {
                        g_allowNoDataMessageBoxUntilMs.store(0, std::memory_order_release);
                        g_messageBoxShowingOurConfirm.store(true, std::memory_order_release);
                    } else {
                        g_messageBoxShowingOurConfirm.store(false, std::memory_order_release);
                    }
                }

                if (showLike && !allowThis) {
                    bool suppressNow =
                        g_removeConfirmOpen.load(std::memory_order_acquire) ||
                        g_removeConfirmQueued.load(std::memory_order_acquire) ||
                        ShouldForceHideMessageBoxNow();
                    if (!suppressNow) {
                        auto* menu = GetActiveEnchantConstructMenu();
                        suppressNow = menu && menu->currentCategory == RE::CraftingSubMenus::EnchantConstructMenu::Category::Disenchant &&
                                      ShouldSuppressVanillaDisenchantPrompt(menu);
                    }

                    if (!a_message.data) {
                        if (ShouldAllowNoDataMessageBoxNow()) {
                            return ProcessMessage_Original(a_this, a_message);
                        }
                        if (suppressNow) {
                            if (auto* queue = RE::UIMessageQueue::GetSingleton()) {
                                queue->AddMessage(RE::MessageBoxMenu::MENU_NAME, RE::UI_MESSAGE_TYPE::kForceHide, nullptr);
                            }
                            return RE::UI_MESSAGE_RESULTS::kIgnore;
                        }
                        return ProcessMessage_Original(a_this, a_message);
                    }

                    if (suppressNow) {
                        if (auto* queue = RE::UIMessageQueue::GetSingleton()) {
                            queue->AddMessage(RE::MessageBoxMenu::MENU_NAME, RE::UI_MESSAGE_TYPE::kForceHide, nullptr);
                        }
                        g_messageBoxShowingOurConfirm.store(false, std::memory_order_release);
                        return RE::UI_MESSAGE_RESULTS::kIgnore;
                    }
                }

                return ProcessMessage_Original(a_this, a_message);
            }

            static void Install()
            {
                REL::Relocation<std::uintptr_t> vtbl{ RE::VTABLE_MessageBoxMenu[0] };
                ProcessMessage_Original = vtbl.write_vfunc(0x4, ProcessMessage_Thunk);
            }

            static inline REL::Relocation<decltype(ProcessMessage_Thunk)> ProcessMessage_Original;
        };

        struct MessageBoxMenuPreDisplayHook
        {
            static void PreDisplay_Thunk(RE::MessageBoxMenu* a_this)
            {
                const bool suppressNow =
                    (g_removeConfirmOpen.load(std::memory_order_acquire) ||
                     g_removeConfirmQueued.load(std::memory_order_acquire) ||
                     ShouldForceHideMessageBoxNow());

                const bool allowNow =
                    g_messageBoxShowingOurConfirm.load(std::memory_order_acquire) ||
                    ShouldAllowNoDataMessageBoxNow();
                if (suppressNow && !allowNow) {
                    if (auto* queue = RE::UIMessageQueue::GetSingleton()) {
                        queue->AddMessage(RE::MessageBoxMenu::MENU_NAME, RE::UI_MESSAGE_TYPE::kForceHide, nullptr);
                    }
                    return;
                }

                PreDisplay_Original(a_this);
            }

            static void Install()
            {
                REL::Relocation<std::uintptr_t> vtbl{ RE::VTABLE_MessageBoxMenu[0] };
                PreDisplay_Original = vtbl.write_vfunc(0x7, PreDisplay_Thunk);
            }

            static inline REL::Relocation<decltype(PreDisplay_Thunk)> PreDisplay_Original;
        };

        struct MessageBoxMenuPostCreateHook
        {
            static void PostCreate_Thunk(RE::MessageBoxMenu* a_this)
            {
                const bool suppressNow =
                    (g_removeConfirmOpen.load(std::memory_order_acquire) ||
                     g_removeConfirmQueued.load(std::memory_order_acquire) ||
                     ShouldForceHideMessageBoxNow());

                const bool allowNow =
                    g_messageBoxShowingOurConfirm.load(std::memory_order_acquire) ||
                    ShouldAllowNoDataMessageBoxNow();
                if (suppressNow && !allowNow) {
                    if (auto* queue = RE::UIMessageQueue::GetSingleton()) {
                        queue->AddMessage(RE::MessageBoxMenu::MENU_NAME, RE::UI_MESSAGE_TYPE::kForceHide, nullptr);
                    }
                    return;
                }

                PostCreate_Original(a_this);
            }

            static void Install()
            {
                REL::Relocation<std::uintptr_t> vtbl{ RE::VTABLE_MessageBoxMenu[0] };
                PostCreate_Original = vtbl.write_vfunc(0x2, PostCreate_Thunk);
            }

            static inline REL::Relocation<decltype(PostCreate_Thunk)> PostCreate_Original;
        };

        struct ProcessUserEventHook
        {
            static bool ProcessUserEvent_Thunk(RE::CraftingSubMenus::EnchantConstructMenu* a_this, RE::BSFixedString* a_control)
            {
                const auto* controlName = (a_control && a_control->data()) ? a_control->data() : nullptr;
                const auto inDisenchantCategory =
                    a_this && a_this->currentCategory == RE::CraftingSubMenus::EnchantConstructMenu::Category::Disenchant;
                const auto isLearn = IsLearnControl(a_control);
                const auto physicalLMBPressed = (::GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;

                if (inDisenchantCategory && ShouldSuppressEnchantInputNow() && isLearn) {
                    return true;
                }

                auto* selectionEntry = inDisenchantCategory ? ResolveDisenchantSelection(a_this) : nullptr;

                auto* selectedItemEntry = (a_this && a_this->selected.item) ? a_this->selected.item.get() : nullptr;
                auto* highlightedItemEntry = GetHighlightedItemEntry(a_this);
                RE::InventoryEntryData* selectedEntryData = (selectedItemEntry && selectedItemEntry->data) ? selectedItemEntry->data : nullptr;
                RE::InventoryEntryData* highlightedEntryData =
                    (highlightedItemEntry && highlightedItemEntry->data) ? highlightedItemEntry->data : nullptr;

                std::optional<std::uint64_t> actionMarkedKey;
                std::optional<MarkSignature> actionMarkedSignature;
                if (inDisenchantCategory) {
                    if (highlightedEntryData && IsEntryMarked(highlightedEntryData, &actionMarkedKey, &actionMarkedSignature)) {} 
                    else if (selectedEntryData && IsEntryMarked(selectedEntryData, &actionMarkedKey, &actionMarkedSignature)) {} 
                    else if (selectionEntry) {
                        (void)IsEntryMarked(selectionEntry, &actionMarkedKey, &actionMarkedSignature);
                    }
                }

                if (inDisenchantCategory && (actionMarkedKey || actionMarkedSignature)) {
                    ForceEnableMarkedDisenchantRows(a_this);
                }

                if (inDisenchantCategory && (actionMarkedKey || actionMarkedSignature)) {
                    if (isLearn) {
                        return true;
                    }
                }

                if (inDisenchantCategory && (actionMarkedKey || actionMarkedSignature)) {
                    const auto mouseSelectLikeControl =
                        (controlName && (_stricmp(controlName, "RightEquip") == 0 || _stricmp(controlName, "LeftEquip") == 0));
                    if (physicalLMBPressed || mouseSelectLikeControl) {
                        if (highlightedItemEntry && highlightedItemEntry->data &&
                            !EntryHasAnyEnchantment(highlightedItemEntry->data)) {
                            return true;
                        }
                        if (highlightedItemEntry) {
                            SelectDisenchantEntryWithoutAction(a_this, highlightedItemEntry);
                        }
                        if (!ShouldSuppressConfirmNow()) {
                            ShowRemoveConfirmation(a_this, actionMarkedKey, actionMarkedSignature);
                        }
                        return true;
                    }
                }

                if (inDisenchantCategory && controlName &&
                    (_stricmp(controlName, "RightEquip") == 0 || _stricmp(controlName, "LeftEquip") == 0)) {
                    if (highlightedItemEntry && highlightedItemEntry->data &&
                        !EntryHasAnyEnchantment(highlightedItemEntry->data)) {
                        return true;
                    }
                    if (highlightedItemEntry) {
                        SelectDisenchantEntryWithoutAction(a_this, highlightedItemEntry);
                    }
                    return true;
                }

                if (inDisenchantCategory && isLearn && !(actionMarkedKey || actionMarkedSignature)) {
                    const auto hasVisibleTarget =
                        (highlightedEntryData != nullptr) || (selectedEntryData != nullptr) || (selectionEntry != nullptr);
                    const auto activeTargetHasEnchantment =
                        highlightedEntryData ? EntryHasAnyEnchantment(highlightedEntryData) :
                        (selectedEntryData ? EntryHasAnyEnchantment(selectedEntryData) :
                                             (selectionEntry && EntryHasAnyEnchantment(selectionEntry)));
                    if (hasVisibleTarget && !activeTargetHasEnchantment) {
                        return true;
                    }
                }

                return g_processUserEventOriginal(a_this, a_control);
            }

            static void Install()
            {
                REL::Relocation<std::uintptr_t> vtbl{ RE::VTABLE_CraftingSubMenus__EnchantConstructMenu[0] };
                g_processUserEventOriginal = vtbl.write_vfunc(0x5, ProcessUserEvent_Thunk);
            }
        };

        struct CraftRunHook
        {
            static void Run_Thunk(RE::CraftingSubMenus::EnchantConstructMenu::EnchantMenuCraftCallback* a_this, RE::IMessageBoxCallback::Message a_msg)
            {
                auto keyToMark = (a_this && a_this->subMenu) ? GetSelectedEntryMarkKey(a_this->subMenu) : std::nullopt;
                auto signatureToMark = (a_this && a_this->subMenu) ? GetSelectedEntryMarkSignature(a_this->subMenu) : std::nullopt;

                Run_Original(a_this, a_msg);

                if (!keyToMark && a_this && a_this->subMenu) {
                    keyToMark = GetSelectedEntryMarkKey(a_this->subMenu);
                }

                if (!signatureToMark && a_this && a_this->subMenu) {
                    signatureToMark = GetSelectedEntryMarkSignature(a_this->subMenu);
                }

                if (keyToMark && ItemHasExtraEnchantment(*keyToMark)) {
                    MarkItem(*keyToMark);
                }

                if (signatureToMark && ItemHasExtraEnchantment(*signatureToMark)) {
                    MarkItem(*signatureToMark);
                }
            }

            static void Install()
            {
                REL::Relocation<std::uintptr_t> vtbl{ RE::VTABLE_CraftingSubMenus__EnchantConstructMenu__EnchantMenuCraftCallback[0] };
                Run_Original = vtbl.write_vfunc(0x1, Run_Thunk);
            }

            static inline REL::Relocation<decltype(Run_Thunk)> Run_Original;
        };

        struct DisenchantRunHook
        {
            static void Run_Thunk(RE::CraftingSubMenus::EnchantConstructMenu::EnchantMenuDisenchantCallback* a_this, RE::IMessageBoxCallback::Message a_msg)
            {
                auto* subMenu = a_this ? a_this->subMenu : nullptr;
                auto* selectionEntry = subMenu ? ResolveDisenchantSelection(subMenu) : nullptr;
                const auto keyBefore = selectionEntry ? FindMarkedKeyInEntry(selectionEntry) : std::nullopt;
                const auto signatureBefore = selectionEntry ? GetEntryMarkSignature(selectionEntry) : std::nullopt;

                auto* selectedItemEntry = (subMenu && subMenu->selected.item) ? subMenu->selected.item.get() : nullptr;
                auto* highlightedItemEntry = subMenu ? GetHighlightedItemEntry(subMenu) : nullptr;
                auto* selectedEntryData = (selectedItemEntry && selectedItemEntry->data) ? selectedItemEntry->data : nullptr;
                auto* highlightedEntryData = (highlightedItemEntry && highlightedItemEntry->data) ? highlightedItemEntry->data : nullptr;

                const auto isMarked = selectionEntry && IsEntryMarked(selectionEntry);
                const auto selectedMarked = selectedEntryData && IsEntryMarked(selectedEntryData);
                const auto highlightedMarked = highlightedEntryData && IsEntryMarked(highlightedEntryData);

                const auto staleSelectionNoEnchant =
                    (selectionEntry && !EntryHasAnyEnchantment(selectionEntry)) ||
                    (selectedEntryData && !EntryHasAnyEnchantment(selectedEntryData)) ||
                    (highlightedEntryData && !EntryHasAnyEnchantment(highlightedEntryData));
                const auto shouldBlockVanilla = isMarked || selectedMarked || highlightedMarked || staleSelectionNoEnchant;

                if (shouldBlockVanilla) {
                    return;
                }

                Run_Original(a_this, a_msg);

                if (keyBefore && IsMarked(*keyBefore) &&
                    (!ItemExistsInPlayerInventory(*keyBefore) || !ItemHasExtraEnchantment(*keyBefore))) {
                    UnmarkItem(*keyBefore);
                }

                if (signatureBefore && IsMarked(*signatureBefore) &&
                    (!ItemExistsInPlayerInventory(*signatureBefore) || !ItemHasExtraEnchantment(*signatureBefore))) {
                    UnmarkItem(*signatureBefore);
                }
            }

            static void Install()
            {
                REL::Relocation<std::uintptr_t> vtbl{ RE::VTABLE_CraftingSubMenus__EnchantConstructMenu__EnchantMenuDisenchantCallback[0] };
                Run_Original = vtbl.write_vfunc(0x1, Run_Thunk);
            }

            static inline REL::Relocation<decltype(Run_Thunk)> Run_Original;
        };

        void SaveCallback(SKSE::SerializationInterface* a_serialization)
        {
            std::scoped_lock lk(g_markedItemsLock);
            if (!a_serialization->OpenRecord(kSerializationRecordType, kSerializationVersion)) {
                SKSE::log::error("Failed to open serialization record");
                return;
            }

            const auto count = static_cast<std::uint32_t>(g_markedItems.size());
            if (!a_serialization->WriteRecordData(count)) {
                SKSE::log::error("Failed to write marked item count");
                return;
            }

            for (const auto key : g_markedItems) {
                if (!a_serialization->WriteRecordData(key)) {
                    SKSE::log::error("Failed to write marked key {:016X}", key);
                    return;
                }
            }

            const auto signatureCount = static_cast<std::uint32_t>(g_markedSignatures.size());
            if (!a_serialization->WriteRecordData(signatureCount)) {
                SKSE::log::error("Failed to write marked signature count");
                return;
            }

            for (const auto& signature : g_markedSignatures) {
                const auto raw = static_cast<std::uint64_t>(signature);
                const auto objectFormID = static_cast<std::uint32_t>(raw >> 32u);
                const auto enchantmentFormID = static_cast<std::uint32_t>(raw & 0xFFFFFFFFu);
                if (!a_serialization->WriteRecordData(objectFormID) ||
                    !a_serialization->WriteRecordData(enchantmentFormID)) {
                    SKSE::log::error("Failed to write marked signature {:08X}:{:08X}", objectFormID, enchantmentFormID);
                    return;
                }
            }
        }

        void LoadCallback(SKSE::SerializationInterface* a_serialization)
        {
            std::uint32_t type = 0;
            std::uint32_t version = 0;
            std::uint32_t length = 0;

            std::scoped_lock lk(g_markedItemsLock);
            g_markedItems.clear();
            g_markedSignatures.clear();

            while (a_serialization->GetNextRecordInfo(type, version, length)) {
                if (type != kSerializationRecordType || (version != 1 && version != kSerializationVersion)) {
                    continue;
                }

                std::uint32_t count = 0;
                if (!a_serialization->ReadRecordData(count)) {
                    SKSE::log::error("Failed to read marked item count");
                    return;
                }

                for (std::uint32_t i = 0; i < count; ++i) {
                    std::uint64_t key = 0;
                    if (!a_serialization->ReadRecordData(key)) {
                        SKSE::log::error("Failed to read marked item key #{}", i);
                        return;
                    }

                    g_markedItems.insert(key);
                }

                if (version >= 2) {
                    std::uint32_t signatureCount = 0;
                    if (!a_serialization->ReadRecordData(signatureCount)) {
                        SKSE::log::error("Failed to read marked signature count");
                        return;
                    }

                    for (std::uint32_t i = 0; i < signatureCount; ++i) {
                        std::uint32_t objectFormID = 0;
                        std::uint32_t enchantmentFormID = 0;
                        if (!a_serialization->ReadRecordData(objectFormID) ||
                            !a_serialization->ReadRecordData(enchantmentFormID)) {
                            SKSE::log::error("Failed to read marked signature #{}", i);
                            return;
                        }

                        g_markedSignatures.insert(MakeMarkSignature(objectFormID, enchantmentFormID));
                    }
                }

                return;
            }
        }

        void RevertCallback(SKSE::SerializationInterface*)
        {
            std::scoped_lock lk(g_markedItemsLock);
            g_markedItems.clear();
            g_markedSignatures.clear();
        }
    }

    bool Install()
    {
        ItemChangeSetDataHook::Install();
        ItemChangeActivateHook::Install();
        ProcessUserEventHook::Install();
        CraftRunHook::Install();
        DisenchantRunHook::Install();
        MessageBoxMenuProcessMessageHook::Install();
        MessageBoxMenuPostCreateHook::Install();
        MessageBoxMenuPreDisplayHook::Install();
        if (auto* input = RE::BSInputDeviceManager::GetSingleton()) {
            input->AddEventSink(&g_removeHotkeySink);
        } else {
            SKSE::log::error("Failed to install input sink (BSInputDeviceManager singleton null)");
        }
        return true;
    }

    void RegisterSerialization()
    {
        auto* serialization = SKSE::GetSerializationInterface();
        serialization->SetUniqueID('RFDI');
        serialization->SetSaveCallback(SaveCallback);
        serialization->SetLoadCallback(LoadCallback);
        serialization->SetRevertCallback(RevertCallback);
    }
}
