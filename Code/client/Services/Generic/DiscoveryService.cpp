#include <TiltedOnlinePCH.h>

#include <Services/DiscoveryService.h>
#include <Games/TES.h>

#include <Games/References.h>
#include <Games/Skyrim/ActorSanity.h>

#include <Forms/TESObjectCELL.h>
#include <Forms/TESWorldSpace.h>
#include <Forms/TESNPC.h>

#include <Events/ActorAddedEvent.h>
#include <Events/ActorRemovedEvent.h>
#include <Events/PreUpdateEvent.h>
#include <Events/GridCellChangeEvent.h>
#include <Events/CellChangeEvent.h>
#include <Events/LocationChangeEvent.h>
#include <Events/ConnectedEvent.h>
#include <Events/ConnectionErrorEvent.h>

#include <World.h>

DiscoveryService::DiscoveryService(World& aWorld, entt::dispatcher& aDispatcher) noexcept
    : m_world(aWorld)
    , m_dispatcher(aDispatcher)
{
    m_preUpdateConnection = m_dispatcher.sink<PreUpdateEvent>().connect<&DiscoveryService::OnUpdate>(this);
    m_connectedConnection = m_dispatcher.sink<ConnectedEvent>().connect<&DiscoveryService::OnConnected>(this);

    EventDispatcherManager::Get()->loadGameEvent.RegisterSink(this);
}

void DiscoveryService::VisitCell(bool aForceTrigger) noexcept
{
    const PlayerCharacter* pPlayer = PlayerCharacter::Get();
    if (!pPlayer)
        return;

    if (pPlayer->GetWorldSpace())
        VisitExteriorCell(aForceTrigger);
    else if (pPlayer->GetParentCell())
        VisitInteriorCell(aForceTrigger);

    // exactly how the game does it too
    if (m_pLocation != pPlayer->locationForm)
    {
        m_dispatcher.trigger(LocationChangeEvent());
        m_pLocation = pPlayer->locationForm;
    }
}

void DiscoveryService::VisitExteriorCell(bool aForceTrigger) noexcept
{
    const PlayerCharacter* pPlayer = PlayerCharacter::Get();
    const auto pWorldSpace = pPlayer->GetWorldSpace();

    m_interiorCellId = 0;

    const TES* pTES = TES::Get();
    const uint32_t worldSpaceId = pWorldSpace->formID;
    const GridCellCoords gameCurrentGrid(pTES->currentGridX, pTES->currentGridY);
    const GridCellCoords gameCenterGrid(pTES->centerGridX, pTES->centerGridY);

    if (m_worldSpaceId != worldSpaceId || aForceTrigger)
    {
        DetectGridCellChange(pWorldSpace, true);
        // If the world space changes, then we want to send out a CellChangeEvent out too.
        aForceTrigger = true;
    }
    else if (gameCenterGrid != m_centerGrid)
    {
        DetectGridCellChange(pWorldSpace, false);
    }

    if (gameCurrentGrid != m_currentGrid || aForceTrigger)
    {
        CellChangeEvent cellChangeEvent{};

        if (!m_world.GetModSystem().GetServerModId(pWorldSpace->formID, cellChangeEvent.WorldSpaceId))
        {
            spdlog::error("Failed to find world space id for form id {:X}", pWorldSpace->formID);
            return;
        }

        TESObjectCELL* pCell = pPlayer->GetParentCellEx();
        if (!pCell)
            pCell = ModManager::Get()->GetCellFromCoordinates(gameCurrentGrid.X, gameCurrentGrid.Y, pWorldSpace, false);

        if (!m_world.GetModSystem().GetServerModId(pCell->formID, cellChangeEvent.CellId))
        {
            spdlog::error("Failed to find cell id for form id {:X}", pCell->formID);
            return;
        }

        cellChangeEvent.CurrentCoords = gameCurrentGrid;

        m_dispatcher.trigger(cellChangeEvent);

        m_currentGrid = gameCurrentGrid;
    }
}

void DiscoveryService::VisitInteriorCell(bool aForceTrigger) noexcept
{
    ResetCachedCellData();

    const uint32_t cellId = PlayerCharacter::Get()->GetParentCell()->formID;
    if (m_interiorCellId != cellId || aForceTrigger)
    {
        CellChangeEvent cellChangeEvent{};

        if (!m_world.GetModSystem().GetServerModId(cellId, cellChangeEvent.CellId))
        {
            spdlog::error("Failed to find cell id {:X}", cellId);
            return;
        }

        m_dispatcher.trigger(cellChangeEvent);
        m_interiorCellId = cellId;
    }
}

void DiscoveryService::DetectGridCellChange(TESWorldSpace* aWorldSpace, bool aNewCellGrid) noexcept
{
    GridCellChangeEvent changeEvent{};

    const uint32_t worldSpaceId = aWorldSpace->formID;
    changeEvent.WorldSpaceId = worldSpaceId;

    const TES* pTES = TES::Get();
    const int32_t startGridX = pTES->centerGridX - GridCellCoords::m_gridsToLoad / 2;
    const int32_t startGridY = pTES->centerGridY - GridCellCoords::m_gridsToLoad / 2;

    for (int32_t i = 0; i < GridCellCoords::m_gridsToLoad; ++i)
    {
        for (int32_t j = 0; j < GridCellCoords::m_gridsToLoad; ++j)
        {
            // If it is a new cell grid, don't check for previously loaded cells.
            if (!aNewCellGrid)
            {
                if (GridCellCoords::IsCellInGridCell(m_centerGrid, {startGridX + i, startGridY + j}, false))
                    continue;
            }

            const TESObjectCELL* pCell = ModManager::Get()->GetCellFromCoordinates(startGridX + i, startGridY + j, aWorldSpace, 0);

            if (!pCell)
            {
                spdlog::warn("Cell not found at coordinates ({}, {}) in worldspace {:X}", startGridX + i, startGridY + j, aWorldSpace->formID);
                continue;
            }

            GameId cellId{};
            if (!m_world.GetModSystem().GetServerModId(pCell->formID, cellId))
            {
                spdlog::error("Failed to find cell id for form id {:X}", pCell->formID);
                continue;
            }

            changeEvent.Cells.push_back(cellId);
        }
    }

    TESObjectCELL* pCell = PlayerCharacter::Get()->GetParentCellEx();
    if (!pCell)
        pCell = ModManager::Get()->GetCellFromCoordinates(pTES->currentGridX, pTES->currentGridY, aWorldSpace, false);

    if (!m_world.GetModSystem().GetServerModId(pCell->formID, changeEvent.PlayerCell))
    {
        spdlog::error("Failed to find cell id for form id {:X}", pCell->formID);
        return;
    }

    changeEvent.CenterCoords = m_centerGrid = {pTES->centerGridX, pTES->centerGridY};

    m_dispatcher.trigger(changeEvent);

    m_worldSpaceId = worldSpaceId;
}

void DiscoveryService::VisitForms() noexcept
{
    static Set<uint32_t> s_previousForms;
    s_previousForms = m_forms;

    const auto visitor = [this](TESObjectREFR* apReference)
    {
        const auto formId = apReference->formID;

        if (!m_forms.count(formId))
        {
            m_forms.insert(formId);

            m_dispatcher.enqueue(ActorAddedEvent(formId));
        }
        else
            s_previousForms.erase(formId);

        // Seen this sweep, so whatever gap it had is over.
        m_missedSweeps.erase(formId);
    };

    ProcessLists* const pProcessLists = ProcessLists::Get();
    if (!pProcessLists)
        return;

    /**
     * All four process lists, not only the high one.
     *
     * The game demotes an actor to a middle or low list when it is further away or cheaper to run, and such an
     * actor is still loaded and can be standing in plain sight. Scanning only the high list meant those were
     * never discovered, so ActorAddedEvent never fired for them, they were never registered with the server,
     * and no other player was ever told they existed.
     *
     * That is what happened to two Marauders spawned by a dungeon lever on 2026-08-18: they appeared for the
     * player who pulled it and for nobody else. Which half of the pipeline failed is not a guess, because
     * RequestServerAssignment logs an error whenever it cannot map a form and there was not one in the entire
     * session. The request was never made at all.
     *
     * The GetNiNode() filter still applies, so this only adds actors that actually have 3D.
     */
    for (GameArray<uint32_t>* pBucket : {&pProcessLists->highActorHandleArray, &pProcessLists->middleHighActorHandleArray, &pProcessLists->middleLowActorHandleArray, &pProcessLists->lowActorHandleArray})
    {
        for (uint32_t i = 0; i < pBucket->length; ++i)
        {
            TESObjectREFR* const pRefr = TESObjectREFR::GetByHandle((*pBucket)[i]);
            if (pRefr)
            {
                if (pRefr->GetNiNode())
                {
                    visitor(pRefr);

                    // Diagnostic sweep: catches the frame an actor's pointer fields go bad.
                    ValidateActor(Cast<Actor>(pRefr), "world sweep");
                }
            }
        }
    }

    // Not in actor holder
    visitor(PlayerCharacter::Get());

    /**
     * Removal only after the form has been missing for several sweeps in a row. See
     * DiscoveryService::m_missedSweeps for why a single sweep is not enough evidence.
     */
    constexpr uint32_t kMissedSweepsBeforeRemoval = 5;

    // We dispatch removal events first to prevent needless reallocations
    for (uint32_t formId : s_previousForms)
    {
        if (++m_missedSweeps[formId] < kMissedSweepsBeforeRemoval)
            continue;

        m_missedSweeps.erase(formId);

        m_dispatcher.trigger(ActorRemovedEvent(formId));
        m_forms.erase(formId);
    }

    // Dispatch all adds
    m_dispatcher.update<ActorAddedEvent>();
}

void DiscoveryService::OnUpdate(const PreUpdateEvent& acUpdateEvent) noexcept
{
    TP_UNUSED(acUpdateEvent);

    VisitCell();
    VisitForms();
}

void DiscoveryService::OnConnected(const ConnectedEvent& acEvent) noexcept
{
    // uGridsToLoad should always be 5, as this is what the server enforces
    auto* pSetting = INISettingCollection::Get()->GetSetting("uGridsToLoad:General");
    if (pSetting && pSetting->data != 5)
    {
        ConnectionErrorEvent errorEvent{};
        errorEvent.ErrorDetail = "{\"error\": \"bad_uGridsToLoad\"}";

        m_world.GetRunner().Trigger(errorEvent);
    }

    VisitCell(true);
}

BSTEventResult DiscoveryService::OnEvent(const TESLoadGameEvent*, const EventDispatcher<TESLoadGameEvent>*)
{
    spdlog::info("Finished loading, triggering visit cell");

    const TiltedPhoques::String defaultModlist[7] = {"Skyrim.esm",        "Update.esm",     "Dawnguard.esm",
                                                    "HearthFires.esm",   "Dragonborn.esm", "_ResourcePack.esl",
                                                    "SkyrimTogether.esp"};

    auto& currentModlist = ModManager::Get()->mods;

    bool isModlistEqual = currentModlist.Size() == 7;

    if (isModlistEqual)
    {
        int i = 0;
        for (const auto& currentMod : currentModlist)
        {
            if (currentMod->filename != defaultModlist[i])
            {
                isModlistEqual = false;
                break;
            }

            i++;
        }
    }

    if (!isModlistEqual)
    {
        ConnectionErrorEvent errorEvent{};
        errorEvent.ErrorDetail = "{\"error\": \"non_default_install\"}";

        m_world.GetRunner().Trigger(errorEvent);
    }

    VisitCell(true);

    return BSTEventResult::kOk;
}

void DiscoveryService::ResetCachedCellData() noexcept
{
    m_worldSpaceId = 0;
    m_centerGrid.Reset();
    m_currentGrid.Reset();
}
