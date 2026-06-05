// Copyright Hyperdyne Systems LLC 2026. All Rights Reserved.

#include "ShaderShiftEditor.h"
#include "ShaderShiftSettings.h"
#include "ShaderShift.h"

#include "Misc/FileHelper.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/Paths.h"
#include "Interfaces/IPluginManager.h"
#include "ShaderCore.h"

// Viewport redraw after recompileshaders
#include "Containers/Ticker.h"
#include "Editor.h"
#include "EditorViewportClient.h"
#include "EditorSupportDelegates.h"
#include "ShaderCompiler.h"
#include "RenderingThread.h"
#include "GlobalShader.h"
#include "RendererInterface.h"
#include "EngineModule.h"

// Detail customization for the Recompile button
#include "IDetailCustomization.h"
#include "ISettingsModule.h"
#include "DetailLayoutBuilder.h"
#include "DetailCategoryBuilder.h"
#include "DetailWidgetRow.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/Layout/SSpacer.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Input/SComboBox.h"
#include "PropertyEditorModule.h"

// Toolbar extension
#include "ToolMenus.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "Styling/AppStyle.h"
#include "Styling/SlateTypes.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/Layout/SBorder.h"
#include "Styling/SlateStyleRegistry.h"
#include "Widgets/Docking/SDockTab.h"
#include "Framework/Docking/TabManager.h"

// Source control - used by the persist-mode workflow to check out engine
// shader files into the default changelist when the user is on a source
// engine build (Perforce / Plastic).  Degrades to a no-op when SCC is
// disabled in the project.
#include "ISourceControlModule.h"
#include "ISourceControlProvider.h"
#include "ISourceControlState.h"
#include "SourceControlOperations.h"

#define LOCTEXT_NAMESPACE "FShaderShiftEditorModule"

DEFINE_LOG_CATEGORY_STATIC(LogShaderShiftEditor, Log, All);

namespace
{
	/**
	 * If persist mode is active and source control is enabled, ensure the
	 * registered engine shader files are checked out into the user's default
	 * changelist.  Required for source-engine builds where engine shaders are
	 * tracked in P4/Plastic - without this, our writes hit read-only files or
	 * leave the user with modified-but-not-checked-out files that source
	 * control can't reconcile.
	 *
	 * No-ops when:
	 *   - persist is OFF and we're not in a cook commandlet
	 *   - SCC is disabled in the project
	 *   - none of our registered hooks have files on disk yet
	 *   - all relevant files are already checked out / added / under our own user
	 *
	 * Files that are simply not under SCC at all are silently skipped (the
	 * common case for users on a launcher engine install).
	 */
	void CheckOutPatchedShadersIfPersist()
	{
		if (!FShaderShiftModule::ShouldPersistShaderChanges())
		{
			return;
		}

		ISourceControlModule& SCCModule = ISourceControlModule::Get();
		if (!SCCModule.IsEnabled())
		{
			UE_LOG(LogShaderShiftEditor, Verbose,
				TEXT("Persist + SCC: source control not enabled - skipping checkout"));
			return;
		}

		ISourceControlProvider& Provider = SCCModule.GetProvider();
		if (!Provider.IsAvailable())
		{
			UE_LOG(LogShaderShiftEditor, Warning,
				TEXT("Persist + SCC: provider not available (login state, network?) - "
					 "skipping checkout"));
			return;
		}

		// Gather every registered hook's physical engine shader path.
		FShaderShiftHookRegistry& Registry = FShaderShiftHookRegistry::Get();
		TArray<const FShaderShiftFileHook*> AllHooks;
		Registry.GetAllHooks(AllHooks);

		TArray<FString> CandidatePaths;
		CandidatePaths.Reserve(AllHooks.Num());
		for (const FShaderShiftFileHook* Hook : AllHooks)
		{
			if (!Hook)
			{
				continue;
			}
			const FString PhysicalPath = Hook->GetPhysicalPath();
			if (FPaths::FileExists(PhysicalPath))
			{
				CandidatePaths.Add(FPaths::ConvertRelativePathToFull(PhysicalPath));
			}
		}

		if (CandidatePaths.Num() == 0)
		{
			return;
		}

		// Force-update SCC state for these specific files so we don't act on
		// stale cached state from a previous session.
		TArray<FSourceControlStateRef> States;
		const ECommandResult::Type StateResult = Provider.GetState(
			CandidatePaths, States, EStateCacheUsage::ForceUpdate);

		if (StateResult != ECommandResult::Succeeded)
		{
			UE_LOG(LogShaderShiftEditor, Warning,
				TEXT("Persist + SCC: failed to query state for %d engine shader file(s)"),
				CandidatePaths.Num());
			return;
		}

		TArray<FString> FilesToCheckOut;
		FilesToCheckOut.Reserve(States.Num());

		int32 NotUnderSCC      = 0;
		int32 AlreadyCheckedOut = 0;
		int32 OtherUserOpened  = 0;

		for (int32 i = 0; i < States.Num(); ++i)
		{
			const FSourceControlStateRef& State = States[i];

			if (!State->IsSourceControlled())
			{
				++NotUnderSCC;
				continue;
			}
			if (State->IsCheckedOut() || State->IsAdded())
			{
				++AlreadyCheckedOut;
				continue;
			}
			if (State->IsCheckedOutOther())
			{
				++OtherUserOpened;
				UE_LOG(LogShaderShiftEditor, Warning,
					TEXT("Persist + SCC: %s is checked out by another user - "
						 "patches will not be writable"),
					*CandidatePaths[i]);
				continue;
			}

			FilesToCheckOut.Add(CandidatePaths[i]);
		}

		if (FilesToCheckOut.Num() > 0)
		{
			const ECommandResult::Type CheckoutResult = Provider.Execute(
				ISourceControlOperation::Create<FCheckOut>(),
				FilesToCheckOut);

			if (CheckoutResult == ECommandResult::Succeeded)
			{
				UE_LOG(LogShaderShiftEditor, Log,
					TEXT("Persist + SCC: checked out %d engine shader file(s) into default changelist"),
					FilesToCheckOut.Num());
			}
			else
			{
				UE_LOG(LogShaderShiftEditor, Warning,
					TEXT("Persist + SCC: checkout failed for %d engine shader file(s) "
						 "- writes may hit read-only files"),
					FilesToCheckOut.Num());
			}
		}
		else
		{
			UE_LOG(LogShaderShiftEditor, Verbose,
				TEXT("Persist + SCC: nothing to do (not-tracked: %d, already-open: %d, other-user: %d)"),
				NotUnderSCC, AlreadyCheckedOut, OtherUserOpened);
		}
	}
}

// ============================================================================
// FShaderShiftSettingsDetails - IDetailCustomization
//
// Adds a "Recompile Shaders Now" button to the Project Settings panel.
// UFUNCTION(CallInEditor) does not work on UDeveloperSettings because the
// Project Settings details panel does not use the Actor/Component details
// path that invokes CallInEditor functions.
// ============================================================================

class FShaderShiftSettingsDetails : public IDetailCustomization
{
public:
	static TSharedRef<IDetailCustomization> MakeInstance()
	{
		return MakeShareable(new FShaderShiftSettingsDetails);
	}

	virtual void CustomizeDetails(IDetailLayoutBuilder& DetailBuilder) override
	{
		IDetailCategoryBuilder& Category = DetailBuilder.EditCategory(
			TEXT("Recompilation"),
			LOCTEXT("RecompilationCategory", "Recompilation"));

		Category.AddCustomRow(LOCTEXT("RecompileRow", "Recompile Shaders"))
			.NameContent()
			[
				SNew(STextBlock)
				.Font(IDetailLayoutBuilder::GetDetailFont())
				.Text(LOCTEXT("RecompileLabel", "Manual Recompile"))
			]
			.ValueContent()
			.MaxDesiredWidth(200.f)
			[
				SNew(SButton)
				.HAlign(HAlign_Center)
				.ContentPadding(FMargin(6, 2))
				.Text(LOCTEXT("RecompileButton", "Recompile Shaders Now"))
				.OnClicked_Lambda([]() -> FReply
				{
					if (UShaderShiftSettings* Settings = UShaderShiftSettings::Get())
					{
						Settings->RecompileShadersNow();
					}
					return FReply::Handled();
				})
			];
	}
};

// ============================================================================
// SShaderShiftQuickPanel - toolbar popup window
//
// A polished dark panel with status indicators, dropdowns for the main
// shader settings, and Apply / Restore buttons.
// ============================================================================

class SShaderShiftQuickPanel : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SShaderShiftQuickPanel) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs)
	{
		// Cache settings pointer
		Settings = UShaderShiftSettings::Get();
		check(Settings);

		// Build option arrays
		BuildEnumOptions<ECustomTonemapMode>(TonemapOptions);
		BuildEnumOptions<EAgxLook>(AgxLookOptions);
		BuildEnumOptions<ECustomDiffuseMode>(DiffuseOptions);
		BuildEnumOptions<ECustomSpecMode>(SpecularOptions);
		BuildEnumOptions<ECustomBloomMode>(BloomOptions);

		// Current selections
		CurrentTonemapOption  = FindOption(TonemapOptions,  static_cast<uint8>(Settings->TonemapMode));
		CurrentAgxLookOption  = FindOption(AgxLookOptions,  static_cast<uint8>(Settings->AgxLook));
		CurrentDiffuseOption  = FindOption(DiffuseOptions,  static_cast<uint8>(Settings->DiffuseMode));
		CurrentSpecularOption = FindOption(SpecularOptions, static_cast<uint8>(Settings->SpecularMode));
		CurrentBloomOption    = FindOption(BloomOptions,    static_cast<uint8>(Settings->BloomMode));

		// Colors
		const FLinearColor AccentColor(0.0f, 0.867f, 1.0f, 1.0f); // light blue accent
		const FLinearColor DimTextColor(0.55f, 0.55f, 0.55f, 1.0f);
		const FLinearColor BrightTextColor(0.9f, 0.9f, 0.9f, 1.0f);
		const FLinearColor PanelBgColor(0.08f, 0.08f, 0.09f, 1.0f);
		const FLinearColor SectionBgColor(0.11f, 0.11f, 0.13f, 1.0f);
		const FLinearColor ButtonBgColor(0.14f, 0.14f, 0.16f, 1.0f);

		const float PanelWidth = 420.f;
		const float ComboWidth = 240.f;
		const float LabelWidth = 120.f;
		const FMargin SectionPadding(12.f, 8.f);
		const FMargin RowPadding(0.f, 3.f);

		ChildSlot
		[
			SNew(SBox)
			.WidthOverride(PanelWidth)
			[
				SNew(SBorder)
				.BorderImage(FAppStyle::GetBrush("ToolPanel.DarkGroupBorder"))
				.Padding(0)
				.ColorAndOpacity(FLinearColor::White)
				[
					SNew(SVerticalBox)

					// ===================================================
					// Header bar
					// ===================================================
					+ SVerticalBox::Slot()
					.AutoHeight()
					[
						SNew(SBorder)
						.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
						.Padding(FMargin(16.f, 10.f))
						[
							SNew(SHorizontalBox)

							+ SHorizontalBox::Slot()
							.FillWidth(1.f)
							.VAlign(VAlign_Center)
							[
								SNew(SVerticalBox)

								+ SVerticalBox::Slot()
								.AutoHeight()
								[
									SNew(STextBlock)
									.Text(LOCTEXT("PanelTitle", "ShaderShift"))
									.Font(FCoreStyle::GetDefaultFontStyle("Bold", 16))
									.ColorAndOpacity(BrightTextColor)
								]

								+ SVerticalBox::Slot()
								.AutoHeight()
								.Padding(0, 2, 0, 0)
								[
									SNew(STextBlock)
									.Text(LOCTEXT("PanelSubtitle",
										"Engine shader overrides"))
									.Font(FCoreStyle::GetDefaultFontStyle("Regular", 8))
									.ColorAndOpacity(DimTextColor)
								]
							]

							+ SHorizontalBox::Slot()
							.AutoWidth()
							.VAlign(VAlign_Center)
							[
								// Status badge
								SNew(SBorder)
								.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
								.Padding(FMargin(8.f, 4.f))
								[
									SNew(STextBlock)
									.Text_Lambda([this]()
									{
										return IsAnyHookActive()
											? LOCTEXT("StatusActive", "ACTIVE")
											: LOCTEXT("StatusInactive", "INACTIVE");
									})
									.Font(FCoreStyle::GetDefaultFontStyle("Bold", 9))
									.ColorAndOpacity_Lambda([this, AccentColor, DimTextColor]()
									{
										return IsAnyHookActive()
											? FSlateColor(AccentColor)
											: FSlateColor(DimTextColor);
									})
								]
							]
						]
					]

					// ===================================================
					// Tone Mapping section
					// ===================================================
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(16, 10, 16, 0)
					[
						MakeSectionHeader(
							LOCTEXT("ToneMapHeader", "Tone Mapping"),
							LOCTEXT("ToneMapDesc", "PostProcessCombineLUTs.usf"),
							AccentColor, DimTextColor,
							TEXT("/Engine/Private/PostProcessCombineLUTs.usf"))
					]

					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(16, 6, 16, 0)
					[
						MakeComboRow(
							LOCTEXT("TonemapLabel", "Tonemapper"),
							LabelWidth, ComboWidth, DimTextColor,
							&TonemapOptions, &CurrentTonemapOption,
							this, &SShaderShiftQuickPanel::OnTonemapChanged)
					]

					// AgX Look - only visible when an AgX tonemapper is selected
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(16, 4, 16, 0)
					[
						SAssignNew(AgxLookRow, SBox)
						.Visibility_Lambda([this]()
						{
							return IsAgxTonemapper()
								? EVisibility::Visible
								: EVisibility::Collapsed;
						})
						[
							MakeComboRow(
								LOCTEXT("AgxLookLabel", "AgX Look"),
								LabelWidth, ComboWidth, DimTextColor,
								&AgxLookOptions, &CurrentAgxLookOption,
								this, &SShaderShiftQuickPanel::OnAgxLookChanged)
						]
					]

					// ===================================================
					// Shading Model section
					// ===================================================
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(16, 12, 16, 0)
					[
						MakeSectionHeader(
							LOCTEXT("ShadingHeader", "Shading Model"),
							LOCTEXT("ShadingDesc", "ShadingModels.ush"),
							AccentColor, DimTextColor,
							TEXT("/Engine/Private/ShadingModels.ush"))
					]

					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(16, 6, 16, 0)
					[
						MakeComboRow(
							LOCTEXT("DiffuseLabel", "Diffuse BRDF"),
							LabelWidth, ComboWidth, DimTextColor,
							&DiffuseOptions, &CurrentDiffuseOption,
							this, &SShaderShiftQuickPanel::OnDiffuseChanged)
					]

					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(16, 4, 16, 0)
					[
						MakeComboRow(
							LOCTEXT("SpecularLabel", "Specular BRDF"),
							LabelWidth, ComboWidth, DimTextColor,
							&SpecularOptions, &CurrentSpecularOption,
							this, &SShaderShiftQuickPanel::OnSpecularChanged)
					]

					// ===================================================
					// Bloom section
					// ===================================================
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(16, 12, 16, 0)
					[
						MakeSectionHeader(
							LOCTEXT("BloomHeader", "Bloom"),
							LOCTEXT("BloomDesc", "PostProcessBloom.usf"),
							AccentColor, DimTextColor,
							TEXT("/Engine/Private/PostProcessBloom.usf"))
					]

					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(16, 6, 16, 0)
					[
						MakeComboRow(
							LOCTEXT("BloomLabel", "Bloom Mode"),
							LabelWidth, ComboWidth, DimTextColor,
							&BloomOptions, &CurrentBloomOption,
							this, &SShaderShiftQuickPanel::OnBloomChanged)
					]

					// Preserve-emissive-color checkbox - only meaningful for the
					// non-convolution bloom paths, so collapse it for modes 3/5.
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(16, 4, 16, 0)
					[
						SNew(SBox)
						.Visibility_Lambda([this]()
						{
							if (!CurrentBloomOption.IsValid())
							{
								return EVisibility::Visible;
							}
							const uint8 V = CurrentBloomOption->Value;
							const bool bConv =
								V == static_cast<uint8>(ECustomBloomMode::FFTDiffraction)
								|| V == static_cast<uint8>(ECustomBloomMode::SpencerOcular);
							return bConv ? EVisibility::Collapsed : EVisibility::Visible;
						})
						[
							SNew(SHorizontalBox)
							+ SHorizontalBox::Slot()
							.AutoWidth()
							.VAlign(VAlign_Center)
							[
								SNew(SBox)
								.WidthOverride(LabelWidth)
								.VAlign(VAlign_Center)
								[
									SNew(STextBlock)
									.Text(LOCTEXT("PreserveColorLabel", "Preserve Color"))
									.ToolTipText(LOCTEXT("PreserveColorTip",
										"When enabled, the bloom setup preserves the hue of bright "
										"emissive surfaces so their cores keep their colour through "
										"the tonemapper instead of trending to white "
										"(pre-4.17 Unreal bloom behaviour)."))
									.Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
									.ColorAndOpacity(DimTextColor)
								]
							]
							+ SHorizontalBox::Slot()
							.FillWidth(1.f)
							.VAlign(VAlign_Center)
							[
								SNew(SCheckBox)
								.IsChecked_Lambda([this]()
								{
									return Settings && Settings->bPreserveEmissiveColor
										? ECheckBoxState::Checked
										: ECheckBoxState::Unchecked;
								})
								.OnCheckStateChanged_Lambda([this](ECheckBoxState NewState)
								{
									if (Settings)
									{
										Settings->bPreserveEmissiveColor =
											(NewState == ECheckBoxState::Checked);
									}
								})
							]
						]
					]

					// ===================================================
					// Packaging section
					// ===================================================
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(16, 12, 16, 0)
					[
						SNew(SHorizontalBox)

						+ SHorizontalBox::Slot()
						.FillWidth(1.f)
						.VAlign(VAlign_Center)
						[
							SNew(SVerticalBox)

							+ SVerticalBox::Slot()
							.AutoHeight()
							[
								SNew(STextBlock)
								.Text(LOCTEXT("PackagingHeader", "Packaging"))
								.Font(FCoreStyle::GetDefaultFontStyle("Bold", 10))
							]

							+ SVerticalBox::Slot()
							.AutoHeight()
							[
								SNew(STextBlock)
								.Text(LOCTEXT("PackagingDesc",
									"Keep patched shaders on disk across shutdown"))
								.Font(FCoreStyle::GetDefaultFontStyle("Italic", 7))
								.ColorAndOpacity(DimTextColor)
							]
						]
					]

					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(16, 6, 16, 0)
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						[
							SNew(SBox)
							.WidthOverride(LabelWidth)
							.VAlign(VAlign_Center)
							[
								SNew(STextBlock)
								.Text(LOCTEXT("PersistLabel", "Persist Changes"))
								.ToolTipText(LOCTEXT("PersistTip",
									"When enabled, ShaderShift does NOT revert the patched "
									"engine shaders on editor shutdown, crash, or pre-exit, "
									"and skips its stale-backup recovery on next startup. "
									"Use this when packaging the game.\n\n"
									"Cook commandlets auto-persist regardless of this setting. "
									"While this is on, the engine shader files in your install "
									"remain modified - turn it off and click Restore Engine "
									"Defaults before uninstalling the plugin."))
								.Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
								.ColorAndOpacity(DimTextColor)
							]
						]
						+ SHorizontalBox::Slot()
						.FillWidth(1.f)
						.VAlign(VAlign_Center)
						[
							SNew(SCheckBox)
							.IsChecked_Lambda([this]()
							{
								return Settings && Settings->bPersistShaderChangesInEngine
									? ECheckBoxState::Checked
									: ECheckBoxState::Unchecked;
							})
							.OnCheckStateChanged_Lambda([this](ECheckBoxState NewState)
							{
								if (Settings)
								{
									const bool bNewValue =
										(NewState == ECheckBoxState::Checked);
									Settings->bPersistShaderChangesInEngine = bNewValue;

									// Mirror into the runtime module so the
									// shutdown/crash handlers honor the new
									// value without waiting for a settings-panel
									// PostEditChangeProperty round-trip.
									FShaderShiftModule::SetPersistShaderChanges(bNewValue);

									Settings->SaveConfig();

									// On OFF→ON transition (now that the runtime
									// flag is updated), check out the engine
									// shader files for source-engine builds.
									if (bNewValue)
									{
										CheckOutPatchedShadersIfPersist();
									}
								}
							})
						]
					]

					// ===================================================
					// Buttons
					// ===================================================
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(16, 14, 16, 14)
					[
						SNew(SHorizontalBox)

						+ SHorizontalBox::Slot()
						.FillWidth(1.f)
						.Padding(0, 0, 4, 0)
						[
							SNew(SButton)
							.HAlign(HAlign_Center)
							.VAlign(VAlign_Center)
							.ContentPadding(FMargin(0, 6))
							.Text(LOCTEXT("ApplyBtn", "Apply"))
							.ToolTipText(LOCTEXT("ApplyTip",
								"Write patched shaders and recompile"))
							.OnClicked(this, &SShaderShiftQuickPanel::OnApplyClicked)
						]

						+ SHorizontalBox::Slot()
						.FillWidth(1.f)
						.Padding(4, 0, 0, 0)
						[
							SNew(SButton)
							.HAlign(HAlign_Center)
							.VAlign(VAlign_Center)
							.ContentPadding(FMargin(0, 6))
							.Text(LOCTEXT("RestoreBtn", "Restore Engine Defaults"))
							.ToolTipText(LOCTEXT("RestoreTip",
								"Revert all engine shaders to originals"))
							.OnClicked(this, &SShaderShiftQuickPanel::OnRestoreClicked)
						]
					]
				]
			]
		];
	}

private:
	// --- Enum option helper ---
	struct FEnumOption
	{
		uint8  Value;
		FText  DisplayName;
	};
	using FEnumOptionPtr = TSharedPtr<FEnumOption>;

	template<typename TEnum>
	void BuildEnumOptions(TArray<FEnumOptionPtr>& OutOptions)
	{
		const UEnum* EnumType = StaticEnum<TEnum>();
		check(EnumType);

		for (int32 i = 0; i < EnumType->NumEnums() - 1; ++i) // -1 to skip _MAX
		{
			FEnumOptionPtr Opt = MakeShared<FEnumOption>();
			Opt->Value       = static_cast<uint8>(i);
			Opt->DisplayName = EnumType->GetDisplayNameTextByIndex(i);
			OutOptions.Add(Opt);
		}
	}

	static FEnumOptionPtr FindOption(
		const TArray<FEnumOptionPtr>& Options, uint8 Value)
	{
		for (const FEnumOptionPtr& Opt : Options)
		{
			if (Opt->Value == Value) return Opt;
		}
		return Options.Num() > 0 ? Options[0] : nullptr;
	}

	// --- Status helpers ---
	bool IsAnyHookActive() const
	{
		FShaderShiftHookRegistry& Registry = FShaderShiftHookRegistry::Get();
		TArray<const FShaderShiftFileHook*> Hooks;
		Registry.GetAllHooks(Hooks);
		for (const FShaderShiftFileHook* Hook : Hooks)
		{
			if (Hook && Hook->IsApplied()) return true;
		}
		return false;
	}

	bool IsHookApplied(const TCHAR* VirtualPath) const
	{
		FShaderShiftHookRegistry& Registry = FShaderShiftHookRegistry::Get();
		const FShaderShiftFileHook* Hook = Registry.FindHook(VirtualPath);
		return Hook && Hook->IsApplied();
	}

	bool IsAgxTonemapper() const
	{
		if (!CurrentTonemapOption.IsValid()) return false;
		// AgX_Punchy = 1
		const uint8 Val = CurrentTonemapOption->Value;
		return Val == static_cast<uint8>(ECustomTonemapMode::AgX_Punchy);
	}

	// --- Section header with status icon ---
	TSharedRef<SWidget> MakeSectionHeader(
		const FText& Title,
		const FText& Subtitle,
		const FLinearColor& AccentColor,
		const FLinearColor& DimColor,
		const TCHAR* HookVirtualPath)
	{
		return SNew(SHorizontalBox)

			// Status indicator circle
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(0, 0, 8, 0)
			[
				SNew(STextBlock)
				.Text(FText::FromString(TEXT("\u2713")))  // checkmark
				.Font(FCoreStyle::GetDefaultFontStyle("Bold", 12))
				.ColorAndOpacity_Lambda(
					[this, HookVirtualPath, AccentColor, DimColor]()
				{
					return IsHookApplied(HookVirtualPath)
						? FSlateColor(AccentColor)
						: FSlateColor(DimColor);
				})
			]

			// Title + subtitle
			+ SHorizontalBox::Slot()
			.FillWidth(1.f)
			.VAlign(VAlign_Center)
			[
				SNew(SVerticalBox)

				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					SNew(STextBlock)
					.Text(Title)
					.Font(FCoreStyle::GetDefaultFontStyle("Bold", 10))
				]

				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					SNew(STextBlock)
					.Text(Subtitle)
					.Font(FCoreStyle::GetDefaultFontStyle("Italic", 7))
					.ColorAndOpacity(DimColor)
				]
			]

			// Applied/Default label
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text_Lambda([this, HookVirtualPath]()
				{
					return IsHookApplied(HookVirtualPath)
						? LOCTEXT("HookPatched", "Patched")
						: LOCTEXT("HookDefault", "Engine Default");
				})
				.Font(FCoreStyle::GetDefaultFontStyle("Regular", 8))
				.ColorAndOpacity_Lambda(
					[this, HookVirtualPath, AccentColor, DimColor]()
				{
					return IsHookApplied(HookVirtualPath)
						? FSlateColor(AccentColor)
						: FSlateColor(DimColor);
				})
			];
	}

	// --- Combo row builder ---
	template<typename TWidget, typename TFunc>
	TSharedRef<SWidget> MakeComboRow(
		const FText& Label,
		float LabelWidth,
		float ComboWidth,
		const FLinearColor& LabelColor,
		TArray<FEnumOptionPtr>* Options,
		FEnumOptionPtr* CurrentSelection,
		TWidget* Widget,
		TFunc Callback)
	{
		return SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				SNew(SBox)
				.WidthOverride(LabelWidth)
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text(Label)
					.Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
					.ColorAndOpacity(LabelColor)
				]
			]
			+ SHorizontalBox::Slot()
			.FillWidth(1.f)
			[
				SNew(SComboBox<FEnumOptionPtr>)
				.OptionsSource(Options)
				.InitiallySelectedItem(*CurrentSelection)
				.OnSelectionChanged_Raw(Widget, Callback)
				.OnGenerateWidget_Static(&SShaderShiftQuickPanel::GenerateComboItem)
				.Content()
				[
					SNew(STextBlock)
					.Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
					.Text_Lambda([CurrentSelection]()
					{
						return (*CurrentSelection).IsValid()
							? (*CurrentSelection)->DisplayName
							: FText::GetEmpty();
					})
				]
			];
	}

	static TSharedRef<SWidget> GenerateComboItem(FEnumOptionPtr Item)
	{
		return SNew(STextBlock)
			.Text(Item->DisplayName)
			.Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
			.Margin(FMargin(4, 2));
	}

	// --- Selection callbacks ---
	void OnTonemapChanged(FEnumOptionPtr NewValue, ESelectInfo::Type)
	{
		if (NewValue.IsValid() && Settings)
		{
			CurrentTonemapOption = NewValue;
			Settings->TonemapMode = static_cast<ECustomTonemapMode>(NewValue->Value);
		}
	}

	void OnAgxLookChanged(FEnumOptionPtr NewValue, ESelectInfo::Type)
	{
		if (NewValue.IsValid() && Settings)
		{
			CurrentAgxLookOption = NewValue;
			Settings->AgxLook = static_cast<EAgxLook>(NewValue->Value);
		}
	}

	void OnDiffuseChanged(FEnumOptionPtr NewValue, ESelectInfo::Type)
	{
		if (NewValue.IsValid() && Settings)
		{
			CurrentDiffuseOption = NewValue;
			Settings->DiffuseMode = static_cast<ECustomDiffuseMode>(NewValue->Value);
		}
	}

	void OnSpecularChanged(FEnumOptionPtr NewValue, ESelectInfo::Type)
	{
		if (NewValue.IsValid() && Settings)
		{
			CurrentSpecularOption = NewValue;
			Settings->SpecularMode = static_cast<ECustomSpecMode>(NewValue->Value);
		}
	}

	void OnBloomChanged(FEnumOptionPtr NewValue, ESelectInfo::Type)
	{
		if (NewValue.IsValid() && Settings)
		{
			CurrentBloomOption = NewValue;
			Settings->BloomMode = static_cast<ECustomBloomMode>(NewValue->Value);
		}
	}

	// --- Button callbacks ---
	FReply OnApplyClicked()
	{
		if (Settings)
		{
			Settings->SyncEnumsToDefines();
			Settings->SaveConfig();

			FShaderShiftEditorModule& EditorModule =
				FModuleManager::GetModuleChecked<FShaderShiftEditorModule>(
					TEXT("ShaderShiftEditor"));
			EditorModule.ForceRecompile();
		}
		return FReply::Handled();
	}

	FReply OnRestoreClicked()
	{
		// Reset every user-facing setting to its engine-default value.
		// All four mode enums use the integer 0 to mean "engine default",
		// AgxLook 0 is its neutral preset, and bPreserveEmissiveColor
		// off matches stock UE behaviour.  Without this reset the
		// dropdowns kept showing whatever the user had selected, and
		// the next PostEditChangeProperty would re-apply the old values
		// onto the engine shaders we just reverted - which made the
		// Restore button look like it did nothing.
		if (Settings)
		{
			Settings->TonemapMode            = ECustomTonemapMode::ACES_Filmic;
			Settings->AgxLook                = EAgxLook::Default;
			Settings->DiffuseMode            = ECustomDiffuseMode::EngineDefault;
			Settings->SpecularMode           = ECustomSpecMode::EngineDefault;
			Settings->BloomMode              = ECustomBloomMode::EngineDefault;
			Settings->bPreserveEmissiveColor = false;

			// Repoint the QuickPanel's current-selection pointers so the
			// combo-box text-lambda renders the updated state on next
			// paint.  The Construct-time InitiallySelectedItem only set
			// the SComboBox's initial state; subsequent visual updates
			// come from the Text_Lambda reading *CurrentSelection.
			CurrentTonemapOption  = FindOption(TonemapOptions,  static_cast<uint8>(Settings->TonemapMode));
			CurrentAgxLookOption  = FindOption(AgxLookOptions,  static_cast<uint8>(Settings->AgxLook));
			CurrentDiffuseOption  = FindOption(DiffuseOptions,  static_cast<uint8>(Settings->DiffuseMode));
			CurrentSpecularOption = FindOption(SpecularOptions, static_cast<uint8>(Settings->SpecularMode));
			CurrentBloomOption    = FindOption(BloomOptions,    static_cast<uint8>(Settings->BloomMode));

			// Persist the reset values so the runtime module's
			// EarlyApplySavedSettings reads them on next editor startup
			// and doesn't re-patch the engine shaders with stale values.
			Settings->SaveConfig();
		}

		// Now revert the patched shader files on disk back to the
		// engine originals.
		FShaderShiftHookRegistry& Registry = FShaderShiftHookRegistry::Get();
		const int32 Reverted = Registry.RevertAllHooks();

		UE_LOG(LogShaderShiftEditor, Log,
			TEXT("Quick Panel - Restored %d hook(s) to engine defaults (settings reset)"),
			Reverted);

		FShaderShiftEditorModule& EditorModule =
			FModuleManager::GetModuleChecked<FShaderShiftEditorModule>(
				TEXT("ShaderShiftEditor"));
		EditorModule.ForceRecompile();

		return FReply::Handled();
	}

	// --- State ---
	UShaderShiftSettings* Settings = nullptr;

	TArray<FEnumOptionPtr> TonemapOptions;
	TArray<FEnumOptionPtr> AgxLookOptions;
	TArray<FEnumOptionPtr> DiffuseOptions;
	TArray<FEnumOptionPtr> SpecularOptions;
	TArray<FEnumOptionPtr> BloomOptions;

	FEnumOptionPtr CurrentTonemapOption;
	FEnumOptionPtr CurrentAgxLookOption;
	FEnumOptionPtr CurrentDiffuseOption;
	FEnumOptionPtr CurrentSpecularOption;
	FEnumOptionPtr CurrentBloomOption;

	TSharedPtr<SBox> AgxLookRow;
};

// ============================================================================
// Toolbar registration helpers
// ============================================================================

static const FName ShaderShiftTabName("ShaderShiftQuickPanel");
static TWeakPtr<SDockTab> GShaderShiftQuickTab;

static void SpawnQuickPanel()
{
	if (!FGlobalTabmanager::Get()->HasTabSpawner(ShaderShiftTabName))
	{
		FGlobalTabmanager::Get()->RegisterNomadTabSpawner(ShaderShiftTabName, FOnSpawnTab::CreateLambda([](const FSpawnTabArgs& Args)
		{
			TSharedRef<SDockTab> Tab = SNew(SDockTab)
				.TabRole(ETabRole::NomadTab)[
					SNew(SShaderShiftQuickPanel)
				];

			GShaderShiftQuickTab = Tab;
			return Tab;
		}))
		.SetDisplayName(LOCTEXT("QuickWindowTitle", "ShaderShift"))
		.SetMenuType(ETabSpawnerMenuType::Hidden);
	}

	FGlobalTabmanager::Get()->TryInvokeTab(ShaderShiftTabName);
}

static void RegisterToolbarButton()
{
	UToolMenus* ToolMenus = UToolMenus::Get();
	if (!ToolMenus)
	{
		return;
	}

	UToolMenu* Menu = ToolMenus->ExtendMenu(
		"LevelEditor.LevelEditorToolBar.PlayToolBar");
	if (!Menu)
	{
		return;
	}

	FToolMenuSection& Section = Menu->FindOrAddSection("ShaderShift");

	Section.AddEntry(FToolMenuEntry::InitToolBarButton(
		"ShaderShiftQuickOptions",
		FUIAction(FExecuteAction::CreateStatic(&SpawnQuickPanel)),
		LOCTEXT("ToolbarLabel", "ShaderShift"),
		LOCTEXT("ToolbarTooltip",
			"Open ShaderShift Quick Options - switch tonemapper and BRDF settings"),
		FSlateIcon(FName("ShaderShiftStyle"),
			"ShaderShift.ToolbarIcon")//"LevelEditor.GameSettings")
	));
}

static void UnregisterToolbarButton()
{
	UToolMenus* ToolMenus = UToolMenus::Get();
	if (ToolMenus)
	{
		ToolMenus->RemoveSection(
			"LevelEditor.LevelEditorToolBar.PlayToolBar",
			"ShaderShift");
	}
}

// ============================================================================
// UShaderShiftSettings
// ============================================================================

UShaderShiftSettings::UShaderShiftSettings()
{
}

UShaderShiftSettings* UShaderShiftSettings::Get()
{
	return GetMutableDefault<UShaderShiftSettings>();
}

void UShaderShiftSettings::SyncEnumsToDefines()
{
	TMap<FString, FString> EnumValues;
	EnumValues.Add(TEXT("CUSTOM_TONEMAP_MODE"),         FString::FromInt(static_cast<int32>(TonemapMode)));
	EnumValues.Add(TEXT("AGX_LOOK"),                    FString::FromInt(static_cast<int32>(AgxLook)));
	EnumValues.Add(TEXT("CUSTOM_DIFFUSE_MODE"),         FString::FromInt(static_cast<int32>(DiffuseMode)));
	EnumValues.Add(TEXT("CUSTOM_SPEC_MODE"),            FString::FromInt(static_cast<int32>(SpecularMode)));
	EnumValues.Add(TEXT("CUSTOM_BLOOM_MODE"),           FString::FromInt(static_cast<int32>(BloomMode)));
	EnumValues.Add(TEXT("CUSTOM_BLOOM_PRESERVE_COLOR"), bPreserveEmissiveColor ? TEXT("1") : TEXT("0"));

	for (FShaderShiftHookConfig& Hook : ShaderHooks)
	{
		for (FShaderShiftDefineEntry& Def : Hook.Defines)
		{
			if (const FString* EnumVal = EnumValues.Find(Def.DefineName))
			{
				Def.Value = *EnumVal;
			}
		}
	}
}

void UShaderShiftSettings::PostEditChangeProperty(
	FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	SyncEnumsToDefines();

	// Push the persist flag into the runtime module so its shutdown / crash
	// handlers see the up-to-date value.  The runtime module reads this flag
	// from its own cached static - UDeveloperSettings is not safe to access
	// from the OnShutdownAfterError lambda context.
	FShaderShiftModule::SetPersistShaderChanges(bPersistShaderChangesInEngine);

	// If persist just became active, check out the engine shader files for
	// source-engine builds (Perforce / Plastic).  Helper is a no-op when
	// persist is OFF or SCC is disabled.  Ordered before ApplyAndRecompileIfNeeded
	// so the checkout completes before any write attempts.
	CheckOutPatchedShadersIfPersist();

	UE_LOG(LogShaderShiftEditor, Log, TEXT("Settings changed - syncing hooks"));

	FShaderShiftEditorModule& EditorModule =
		FModuleManager::GetModuleChecked<FShaderShiftEditorModule>(TEXT("ShaderShiftEditor"));

	const int32 WrittenCount = EditorModule.ApplyAndRecompileIfNeeded();

	if (WrittenCount == 0)
	{
		UE_LOG(LogShaderShiftEditor, Log,
			TEXT("  No shader files changed - skipping recompile"));
	}

	SaveConfig();
}

void UShaderShiftSettings::RecompileShadersNow()
{
	SyncEnumsToDefines();

	FShaderShiftEditorModule& EditorModule =
		FModuleManager::GetModuleChecked<FShaderShiftEditorModule>(TEXT("ShaderShiftEditor"));

	EditorModule.ForceRecompile();

	SaveConfig();
}

// ============================================================================
// FShaderShiftEditorModule
// ============================================================================

void FShaderShiftEditorModule::StartupModule()
{
	UE_LOG(LogShaderShiftEditor, Log,
		TEXT("ShaderShift Editor module starting up..."));

	TSharedPtr<IPlugin> Plugin =
		IPluginManager::Get().FindPlugin(TEXT("ShaderShift"));
	PluginDirectory = Plugin.IsValid()
		? Plugin->GetBaseDir()
		: FPaths::ProjectPluginsDir() / TEXT("ShaderShift");
	ShadersDirectory = FPaths::Combine(PluginDirectory, TEXT("Shaders"));

	LoadShaderHookConfigs();

	if (UShaderShiftSettings* Settings = UShaderShiftSettings::Get())
	{
		Settings->SyncEnumsToDefines();

		// Sync the runtime module's cached persist flag with the resolved
		// UDeveloperSettings value.  The runtime read its own copy directly
		// from the saved ini at PostConfigInit; this keeps the two sources
		// in agreement once UE's full config hierarchy is available.
		FShaderShiftModule::SetPersistShaderChanges(
			Settings->bPersistShaderChangesInEngine);
	}

	SyncSettingsWithHooks();

	// Register IDetailCustomization for the Recompile button in Project Settings.
	FPropertyEditorModule& PropertyModule =
		FModuleManager::LoadModuleChecked<FPropertyEditorModule>("PropertyEditor");
	PropertyModule.RegisterCustomClassLayout(
		UShaderShiftSettings::StaticClass()->GetFName(),
		FOnGetDetailCustomizationInstance::CreateStatic(
			&FShaderShiftSettingsDetails::MakeInstance));

	// Register Slate style for the toolbar button icon.
	RegisterIconStyle();

	// Register the toolbar button.
	RegisterToolbarButton();

	UE_LOG(LogShaderShiftEditor, Log,
		TEXT("Editor module startup complete"));
}

void FShaderShiftEditorModule::ShutdownModule()
{
	UE_LOG(LogShaderShiftEditor, Log,
		TEXT("ShaderShift Editor module shutting down"));

	// Cancel any pending deferred recompile.
	if (DeferredRecompileHandle.IsValid())
	{
		FTSTicker::RemoveTicker(DeferredRecompileHandle);
		DeferredRecompileHandle.Reset();
	}

	// Unregister detail customization.
	if (FModuleManager::Get().IsModuleLoaded("PropertyEditor"))
	{
		FPropertyEditorModule& PropertyModule =
			FModuleManager::GetModuleChecked<FPropertyEditorModule>("PropertyEditor");
		PropertyModule.UnregisterCustomClassLayout(
			UShaderShiftSettings::StaticClass()->GetFName());
	}

	// Unregister toolbar button.
	UnregisterToolbarButton();

	FSlateStyleRegistry::UnRegisterSlateStyle(*StyleSet);

	// Close the quick panel window if open.
	if (TSharedPtr<SDockTab> Tab = GShaderShiftQuickTab.Pin())
	{
		Tab->RequestCloseTab();
	}

	if (FSlateApplication::IsInitialized())
	{
		FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(ShaderShiftTabName);
	}
}

void FShaderShiftEditorModule::RegisterIconStyle()
{
	StyleSet = MakeShareable(new FSlateStyleSet("ShaderShiftStyle"));

	// Get the plugin directory dynamically
	FString PluginDir = IPluginManager::Get().FindPlugin("ShaderShift")->GetBaseDir();
	FString IconPath = FPaths::Combine(PluginDir, TEXT("Resources/Icon_ShaderShift.png"));

	StyleSet->Set("ShaderShift.ToolbarIcon",
		new FSlateImageBrush(IconPath, FVector2D(50.0f, 50.0f))
	);

	FSlateStyleRegistry::RegisterSlateStyle(*StyleSet);
}

// ---------------------------------------------------------------------------
// ScheduleDeferredRecompile
//
// Instead of issuing "recompileshaders" from inside PostEditChangeProperty
// (which runs in a Slate callback context where FlushRenderingCommands can
// deadlock or produce partial flushes), we defer the work to the next
// game-thread tick.  This matches the timing of typing the console command
// manually, which is known to work and update the viewport correctly.
//
// If multiple property changes fire before the ticker gets a chance to run,
// we coalesce them - bPendingNeedsGlobalRecompile is OR'd so the most
// expensive required recompile mode wins.
// ---------------------------------------------------------------------------

void FShaderShiftEditorModule::ScheduleDeferredRecompile(bool bNeedsGlobalRecompile)
{
	bPendingNeedsGlobalRecompile |= bNeedsGlobalRecompile;

	// If a ticker is already pending, it will pick up the OR'd flag - no need
	// to register another one.
	if (DeferredRecompileHandle.IsValid())
	{
		return;
	}

	DeferredRecompileHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateLambda([this](float /*DeltaTime*/) -> bool
		{
			const bool bNeedsGlobal = bPendingNeedsGlobalRecompile;
			bPendingNeedsGlobalRecompile = false;
			DeferredRecompileHandle.Reset();

			ExecuteRecompileAndRedraw(bNeedsGlobal);

			return false; // one-shot
		}),
		0.0f // fire on the very next tick
	);

	UE_LOG(LogShaderShiftEditor, Log,
		TEXT("Deferred shader recompile scheduled for next tick%s"),
		bPendingNeedsGlobalRecompile ? TEXT(" (global)") : TEXT(""));
}

// ---------------------------------------------------------------------------
// ExecuteRecompileAndRedraw
//
// This runs on a normal game-thread tick - the same context as a console
// command - so the full recompile → flush → redraw pipeline works correctly.
// ---------------------------------------------------------------------------

void FShaderShiftEditorModule::ExecuteRecompileAndRedraw(bool bNeedsGlobalRecompile)
{
	// 1. Flush the virtual shader file cache so UE re-reads our patched files.
	FlushShaderFileCache();

	// 2. Issue the appropriate recompileshaders command(s).
	if (GEngine)
	{
		if (bNeedsGlobalRecompile)
		{
			UE_LOG(LogShaderShiftEditor, Log,
				TEXT("Issuing recompileshaders global + changed (shading model hooks changed)..."));
			GEngine->Exec(nullptr, TEXT("recompileshaders global"));
			GEngine->Exec(nullptr, TEXT("recompileshaders changed"));
		}
		else
		{
			UE_LOG(LogShaderShiftEditor, Log,
				TEXT("Issuing recompileshaders changed..."));
			GEngine->Exec(nullptr, TEXT("recompileshaders changed"));
		}
	}

	// 3. Wait for any async shader compilation to finish.
	if (GShaderCompilingManager)
	{
		UE_LOG(LogShaderShiftEditor, Log, TEXT("Waiting for shader compilation to complete..."));
		GShaderCompilingManager->FinishAllCompilation();
		UE_LOG(LogShaderShiftEditor, Log, TEXT("Shader compilation finished."));
	}

	// 4. Block until the render thread has processed the new shader maps.
	//    This is safe here because we are on a normal game-thread tick,
	//    not inside a Slate callback.
	FlushRenderingCommands();

	// 5. Destroy and reallocate the ViewState on every viewport client.
	//
	//    FEditorViewportClient::ViewState holds per-viewport persistent render
	//    state including the cached color grading LUT generated by
	//    PostProcessCombineLUTs.  After the tonemapper shader is recompiled the
	//    old LUT is stale but the viewport keeps reusing it - this is why
	//    opening a *new* viewport shows the correct result (fresh ViewState)
	//    while existing viewports don't.
	//
	//    Destroying the ViewState and immediately reallocating it forces the
	//    renderer to regenerate the LUT (and all other view-dependent caches)
	//    on the next frame.
	if (GEditor)
	{
		for (FEditorViewportClient* Client : GEditor->GetAllViewportClients())
		{
			if (Client)
			{
				Client->ViewState.Destroy();
				Client->ViewState.Allocate(Client->GetWorld()
					? Client->GetWorld()->GetFeatureLevel()
					: GMaxRHIFeatureLevel);
			}
		}

		UE_LOG(LogShaderShiftEditor, Log,
			TEXT("Reset ViewState on %d viewport client(s) to force LUT regeneration"),
			GEditor->GetAllViewportClients().Num());
	}

	// 6. Redraw all viewports.
	if (GEditor)
	{
		// Invalidate every viewport client - marks surfaces dirty and forces
		// the next draw to rebuild its full pipeline state from the current
		// global shader map.
		for (FEditorViewportClient* Client : GEditor->GetAllViewportClients())
		{
			if (Client)
			{
				Client->Invalidate(/*bInvalidateChildViews=*/true,
				                   /*bInvalidateHitProxies=*/true);
			}
		}

		// Redraw all viewports - OS surface + Slate active timer + bNeedsRedraw.
		GEditor->RedrawAllViewports(/*bInvalidateHitProxies=*/true);
		FEditorSupportDelegates::RedrawAllViewports.Broadcast();

		for (FEditorViewportClient* Client : GEditor->GetAllViewportClients())
		{
			if (Client)
			{
				Client->RedrawRequested(Client->Viewport);
			}
		}

		UE_LOG(LogShaderShiftEditor, Log,
			TEXT("Viewport redraw requested on all %d viewport client(s)"),
			GEditor->GetAllViewportClients().Num());
	}
}

// ---------------------------------------------------------------------------
// ApplyAndRecompileIfNeeded
// ---------------------------------------------------------------------------

int32 FShaderShiftEditorModule::ApplyAndRecompileIfNeeded()
{
	UShaderShiftSettings* Settings = UShaderShiftSettings::Get();
	if (!Settings)
	{
		return 0;
	}

	FShaderShiftHookRegistry& Registry = FShaderShiftHookRegistry::Get();

	// Stamp PendingDefines, bEnabled, and bRequiresGlobalRecompile onto every hook
	for (const FShaderShiftHookConfig& HookConfig : Settings->ShaderHooks)
	{
		FShaderShiftFileHook* Hook = Registry.FindHookMutable(HookConfig.VirtualPath);
		if (Hook)
		{
			Hook->bEnabled                = HookConfig.bEnabled;
			Hook->PendingDefines          = HookConfig.Defines;
			Hook->bRequiresGlobalRecompile = HookConfig.bRequiresGlobalRecompile;
		}
	}

	// In persist mode on a source-engine build, ensure the engine shader files
	// are checked out before we attempt to write to them.  Otherwise P4 would
	// leave them read-only and our SaveStringToFile would fail.
	CheckOutPatchedShadersIfPersist();

	// Apply hooks - only hooks whose on-disk content differs will return WroteDisk
	int32 DiskWriteCount = 0;
	bool bNeedsGlobalRecompile = false;
	const int32 SuccessCount = Registry.ApplyAllEnabledHooks(DiskWriteCount, bNeedsGlobalRecompile);

	UE_LOG(LogShaderShiftEditor, Log,
		TEXT("ApplyAndRecompileIfNeeded - %d hook(s) succeeded, %d wrote new content to disk%s"),
		SuccessCount, DiskWriteCount,
		bNeedsGlobalRecompile ? TEXT(" (global recompile required)") : TEXT(""));

	if (DiskWriteCount > 0)
	{
		if (Settings->bAutoRecompile)
		{
			UE_LOG(LogShaderShiftEditor, Log,
				TEXT("  %d hook(s) changed - scheduling deferred recompile"),
				DiskWriteCount);

			ScheduleDeferredRecompile(bNeedsGlobalRecompile);
		}
		else
		{
			UE_LOG(LogShaderShiftEditor, Log,
				TEXT("  %d hook(s) changed but Auto-Recompile is OFF - use 'Recompile Shaders Now' when ready"),
				DiskWriteCount);
		}
	}
	else
	{
		UE_LOG(LogShaderShiftEditor, Verbose,
			TEXT("  All hooks already current - no recompile needed"));
	}

	return DiskWriteCount;
}

// ---------------------------------------------------------------------------
// ForceRecompile
// ---------------------------------------------------------------------------

void FShaderShiftEditorModule::ForceRecompile()
{
	UShaderShiftSettings* Settings = UShaderShiftSettings::Get();
	if (!Settings)
	{
		return;
	}

	Settings->SyncEnumsToDefines();

	FShaderShiftHookRegistry& Registry = FShaderShiftHookRegistry::Get();

	// Stamp PendingDefines, bEnabled, and bRequiresGlobalRecompile onto every hook
	for (const FShaderShiftHookConfig& HookConfig : Settings->ShaderHooks)
	{
		FShaderShiftFileHook* Hook = Registry.FindHookMutable(HookConfig.VirtualPath);
		if (Hook)
		{
			Hook->bEnabled                = HookConfig.bEnabled;
			Hook->PendingDefines          = HookConfig.Defines;
			Hook->bRequiresGlobalRecompile = HookConfig.bRequiresGlobalRecompile;
		}
	}

	// Same SCC check-out pre-pass as ApplyAndRecompileIfNeeded - see comment there.
	CheckOutPatchedShadersIfPersist();

	int32 DiskWriteCount = 0;
	bool bNeedsGlobalRecompile = false;
	Registry.ApplyAllEnabledHooks(DiskWriteCount, bNeedsGlobalRecompile);

	UE_LOG(LogShaderShiftEditor, Log,
		TEXT("ForceRecompile - scheduling recompile (global=%s)"),
		bNeedsGlobalRecompile ? TEXT("true") : TEXT("false"));

	// Always schedule a recompile even if DiskWriteCount == 0 - the user
	// pressed the button explicitly.
	ScheduleDeferredRecompile(bNeedsGlobalRecompile);
}

// ---------------------------------------------------------------------------
// LoadShaderHookConfigs
// ---------------------------------------------------------------------------

void FShaderShiftEditorModule::LoadShaderHookConfigs()
{
	const FString ConfigPath =
		FPaths::Combine(PluginDirectory, TEXT("Config/ShaderOverride.ini"));

	if (!FPaths::FileExists(ConfigPath))
	{
		UE_LOG(LogShaderShiftEditor, Warning,
			TEXT("Plugin config not found: %s"), *ConfigPath);
		return;
	}

	FConfigFile PluginConfig;
	PluginConfig.Read(ConfigPath);

	const FConfigSection* ShadersSection =
		PluginConfig.FindSection(TEXT("Shaders"));
	if (!ShadersSection)
	{
		UE_LOG(LogShaderShiftEditor, Warning,
			TEXT("No [Shaders] section in %s"), *ConfigPath);
		return;
	}

	TArray<FString> ShaderEntries;
	for (auto It = ShadersSection->CreateConstIterator(); It; ++It)
	{
		if (It->Key == TEXT("+Shader"))
		{
			ShaderEntries.Add(It->Value.GetValue());
		}
	}

	UShaderShiftSettings* Settings = UShaderShiftSettings::Get();
	if (!Settings)
	{
		UE_LOG(LogShaderShiftEditor, Warning,
			TEXT("UShaderShiftSettings CDO not available"));
		return;
	}

	for (const FString& Entry : ShaderEntries)
	{
		FString VirtualPath, TemplateFilename, HookDisplayName;
		FString RequiresGlobalStr;

		FParse::Value(*Entry, TEXT("VirtualPath="),              VirtualPath);
		FParse::Value(*Entry, TEXT("TemplateFilename="),         TemplateFilename);
		FParse::Value(*Entry, TEXT("DisplayName="),              HookDisplayName);
		FParse::Value(*Entry, TEXT("RequiresGlobalRecompile="),  RequiresGlobalStr);

		VirtualPath      = VirtualPath.TrimQuotes();
		TemplateFilename = TemplateFilename.TrimQuotes();
		HookDisplayName  = HookDisplayName.TrimQuotes();
		RequiresGlobalStr = RequiresGlobalStr.TrimQuotes();

		const bool bRequiresGlobal =
			RequiresGlobalStr.Equals(TEXT("true"), ESearchCase::IgnoreCase)
			|| RequiresGlobalStr == TEXT("1");

		if (VirtualPath.IsEmpty() || TemplateFilename.IsEmpty())
		{
			UE_LOG(LogShaderShiftEditor, Warning,
				TEXT("Incomplete shader entry: %s"), *Entry);
			continue;
		}

		// Section header is keyed by the bare filename (no path prefix), to
		// match what the runtime module looks up via GetCleanFilename in
		// EarlyApplySavedSettings.  Without this strip, hooks whose
		// TemplateFilename includes a subdirectory (e.g. "Bloom/Foo.usf")
		// get an empty Defines array and SyncEnumsToDefines silently
		// skips them - the patched on-disk file then keeps its default
		// #define values regardless of the dropdown selection.
		const FString SectionKey = FPaths::GetCleanFilename(TemplateFilename);

		const FConfigSection* DefineSection =
			PluginConfig.FindSection(SectionKey);

		TMap<FString, FShaderShiftDefineEntry> ConfigDefines;

		if (DefineSection)
		{
			for (auto It = DefineSection->CreateConstIterator(); It; ++It)
			{
				if (It->Key != TEXT("+Define"))
				{
					continue;
				}

				const FString& DefEntry = It->Value.GetValue();

				FShaderShiftDefineEntry Def;
				FString DefName, DefDisplay, DefDesc, DefDefault;

				FParse::Value(*DefEntry, TEXT("DefineName="),   DefName);
				FParse::Value(*DefEntry, TEXT("DisplayName="),  DefDisplay);
				FParse::Value(*DefEntry, TEXT("Description="),  DefDesc);
				FParse::Value(*DefEntry, TEXT("DefaultValue="), DefDefault);

				Def.DefineName   = DefName.TrimQuotes();
				Def.DisplayName  = DefDisplay.TrimQuotes();
				Def.Description  = DefDesc.TrimQuotes();
				Def.DefaultValue = DefDefault.TrimQuotes();
				Def.Value        = Def.DefaultValue;

				if (!Def.DefineName.IsEmpty())
				{
					ConfigDefines.Add(Def.DefineName, Def);
				}
			}
		}
		else
		{
			UE_LOG(LogShaderShiftEditor, Log,
				TEXT("No defines section [%s] in config"), *SectionKey);
		}

		FShaderShiftHookConfig* HookConfig = nullptr;
		for (FShaderShiftHookConfig& HC : Settings->ShaderHooks)
		{
			if (HC.VirtualPath == VirtualPath)
			{
				HookConfig = &HC;
				break;
			}
		}

		if (!HookConfig)
		{
			FShaderShiftHookConfig NewConfig;
			NewConfig.DisplayName              = HookDisplayName;
			NewConfig.VirtualPath              = VirtualPath;
			NewConfig.TemplateFilename         = TemplateFilename;
			NewConfig.bEnabled                 = true;
			NewConfig.bRequiresGlobalRecompile = bRequiresGlobal;
			Settings->ShaderHooks.Add(NewConfig);
			HookConfig = &Settings->ShaderHooks.Last();
		}
		else
		{
			if (HookConfig->DisplayName.IsEmpty())
			{
				HookConfig->DisplayName = HookDisplayName;
			}
			HookConfig->TemplateFilename         = TemplateFilename;
			HookConfig->bRequiresGlobalRecompile  = bRequiresGlobal;
		}

		for (auto& Pair : ConfigDefines)
		{
			const FString& DefName                   = Pair.Key;
			const FShaderShiftDefineEntry& ConfigDef = Pair.Value;

			FShaderShiftDefineEntry* Existing = nullptr;
			for (FShaderShiftDefineEntry& D : HookConfig->Defines)
			{
				if (D.DefineName == DefName)
				{
					Existing = &D;
					break;
				}
			}

			if (Existing)
			{
				Existing->DisplayName  = ConfigDef.DisplayName;
				Existing->Description  = ConfigDef.Description;
				Existing->DefaultValue = ConfigDef.DefaultValue;
			}
			else
			{
				HookConfig->Defines.Add(ConfigDef);
			}
		}
	}

	UE_LOG(LogShaderShiftEditor, Log,
		TEXT("Loaded %d shader hook configs from plugin config"),
		ShaderEntries.Num());
}

// ---------------------------------------------------------------------------
// SyncSettingsWithHooks
// ---------------------------------------------------------------------------

void FShaderShiftEditorModule::SyncSettingsWithHooks()
{
	UShaderShiftSettings* Settings = UShaderShiftSettings::Get();
	if (!Settings)
	{
		return;
	}

	FShaderShiftHookRegistry& Registry = FShaderShiftHookRegistry::Get();

	// Ensure every registry hook has a settings entry (covers programmatic hooks)
	TArray<const FShaderShiftFileHook*> AllHooks;
	Registry.GetAllHooks(AllHooks);

	for (const FShaderShiftFileHook* Hook : AllHooks)
	{
		bool bFound = false;
		for (const FShaderShiftHookConfig& HC : Settings->ShaderHooks)
		{
			if (HC.VirtualPath == Hook->VirtualPath)
			{
				bFound = true;
				break;
			}
		}

		if (!bFound)
		{
			FShaderShiftHookConfig NewConfig;
			NewConfig.DisplayName      = Hook->DisplayName;
			NewConfig.VirtualPath      = Hook->VirtualPath;
			NewConfig.TemplateFilename = Hook->Filename;
			NewConfig.bEnabled         = true;
			Settings->ShaderHooks.Add(NewConfig);
		}
	}

	// Stamp PendingDefines, bEnabled, and bRequiresGlobalRecompile onto every hook
	for (const FShaderShiftHookConfig& HookConfig : Settings->ShaderHooks)
	{
		FShaderShiftFileHook* Hook = Registry.FindHookMutable(HookConfig.VirtualPath);
		if (Hook)
		{
			Hook->bEnabled                = HookConfig.bEnabled;
			Hook->PendingDefines          = HookConfig.Defines;
			Hook->bRequiresGlobalRecompile = HookConfig.bRequiresGlobalRecompile;
		}
	}

	// Apply all hooks to disk.  The runtime module (PostConfigInit) already
	// patched files using saved settings before the engine compiled shaders.
	// This call will return AlreadyCurrent for hooks whose on-disk content
	// matches - meaning no recompile is needed (the engine already compiled
	// the patched versions).
	//
	// DiskWriteCount > 0 only if the editor module's UObject-based settings
	// differ from what the runtime module wrote (e.g., the user's saved config
	// was somehow inconsistent, or the runtime module couldn't read GConfig).
	// In that case we schedule a deferred recompile as a fallback.
	int32 DiskWriteCount = 0;
	bool bNeedsGlobalRecompile = false;
	const int32 AppliedCount = Registry.ApplyAllEnabledHooks(DiskWriteCount, bNeedsGlobalRecompile);

	UE_LOG(LogShaderShiftEditor, Log,
		TEXT("SyncSettingsWithHooks - %d / %d hooks applied (%d wrote new disk content)"),
		AppliedCount, Registry.GetHookCount(), DiskWriteCount);

	if (DiskWriteCount > 0)
	{
		UE_LOG(LogShaderShiftEditor, Log,
			TEXT("  Startup: %d hook(s) wrote to disk (settings differed from early apply) - scheduling deferred recompile"),
			DiskWriteCount);

		ScheduleDeferredRecompile(bNeedsGlobalRecompile);
	}
	else
	{
		UE_LOG(LogShaderShiftEditor, Log,
			TEXT("  All hooks already current from early apply - no startup recompile needed"));
	}
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FShaderShiftEditorModule, ShaderShiftEditor)
