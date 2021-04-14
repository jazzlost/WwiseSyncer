#include "WwiseWorkUnitParser.h"

#include "Misc/Paths.h"
#include "Misc/ScopedSlowTask.h"
#include "HAL/FileManager.h"
#include "XmlFile.h"
#include "AkUnrealHelper.h"
#include "WorkUnitXmlVisitor.h"
#include "AssetManagement/AkAssetDatabase.h"

#define LOCTEXT_NAMESPACE "AkAudio"

bool WwiseWorkUnitParser::Parse()
{
	if (!visitor)
	{
		return false;
	}

	auto projectFilePath = AkUnrealHelper::GetWwiseProjectPath();
	auto WwiseAudioPath = AkUnrealHelper::GetSoundBankDirectory() + TEXT("/Windows/SoundbanksInfo.xml");
	auto WwiseAudioSecondPath = AkUnrealHelper::GetSoundBankDirectory() + TEXT("/SoundbanksInfo.xml");

	if (!FPaths::FileExists(projectFilePath))
	{
		return false;
	}

	
	bool IsXmlPathValid = parseBankObjectRef(WwiseAudioPath);
	if (!IsXmlPathValid)
	{
		IsXmlPathValid = parseBankObjectRef(WwiseAudioSecondPath);

		if (!IsXmlPathValid)
		{
			UE_LOG(LogTemp, Warning, TEXT("WwiseSyncer::Can't Open SoundbanksIno.xml"));
		}
	}

	visitor->OnBeginParse();
	projectRootFolder = FPaths::GetPath(projectFilePath) + TEXT("/");
	for (int i = EWwiseItemType::Event; i <= EWwiseItemType::LastWwiseDraggable; ++i)
	{
		const auto CurrentType = static_cast<EWwiseItemType::Type>(i);
		visitor->Init(CurrentType);
		parseFolders(EWwiseItemType::FolderNames[i], CurrentType);
	}
	visitor->End();


	return true;
}

bool WwiseWorkUnitParser::ForceParse()
{
	if (!visitor)
	{
		return false;
	}

	wwuLastPopulateTime.Reset();
	visitor->ForceInit();
	return Parse();
}

void WwiseWorkUnitParser::parseFolders(const FString& FolderName, EWwiseItemType::Type ItemType)
{
	TArray<FString> NewWwus;
	TArray<FString> KnownWwus;
	TArray<FString> WwusToProcess;
	TArray<FString> WwusToRemove;
	FString FullPath = FPaths::Combine(projectRootFolder, FolderName);

	IFileManager::Get().FindFilesRecursive(NewWwus, *FullPath, TEXT("*.wwu"), true, false);

	TMap<FString, FDateTime>& LastPopTimeMap = wwuLastPopulateTime.FindOrAdd(ItemType);
	LastPopTimeMap.GetKeys(KnownWwus);

	// Get lists of files to parse, and files that have been deleted
	NewWwus.Sort();
	KnownWwus.Sort();
	int32 iKnown = 0;
	int32 iNew = 0;

	while (iNew < NewWwus.Num() && iKnown < KnownWwus.Num())
	{
		if (KnownWwus[iKnown] == NewWwus[iNew])
		{
			// File was there and is still there.  Check the FileTimes.
			FDateTime LastPopTime = LastPopTimeMap[KnownWwus[iKnown]];
			FDateTime LastModifiedTime = IFileManager::Get().GetTimeStamp(*NewWwus[iNew]);
			if (LastPopTime < LastModifiedTime)
			{
				WwusToProcess.Add(KnownWwus[iKnown]);
			}
			iKnown++;
			iNew++;
		}
		else if (KnownWwus[iKnown] > NewWwus[iNew])
		{
			// New Wwu detected. Add it to the ToProcess list
			WwusToProcess.Add(NewWwus[iNew]);
			iNew++;
		}
		else
		{
			// A file was deleted. Can't process it now, it would change the array indices.
			WwusToRemove.Add(KnownWwus[iKnown]);
			iKnown++;
		}
	}

	//The remainder from the files found on disk are all new files.
	for (; iNew < NewWwus.Num(); iNew++)
	{
		WwusToProcess.Add(NewWwus[iNew]);
	}

	//All the remainder is deleted.
	while (iKnown < KnownWwus.Num())
	{
		visitor->RemoveWorkUnit(KnownWwus[iKnown]);
		LastPopTimeMap.Remove(KnownWwus[iKnown]);
		iKnown++;
	}

	//Delete those tagged.
	for (FString& ToRemove : WwusToRemove)
	{
		visitor->RemoveWorkUnit(ToRemove);
		LastPopTimeMap.Remove(ToRemove);
	}

	FScopedSlowTask SlowTask(WwusToProcess.Num(), LOCTEXT("AK_PopulatingPicker", "Parsing Wwise Work Unit..."));
	SlowTask.MakeDialog();

	for (FString WwuToProcess : WwusToProcess)
	{
		FString Message = TEXT("Parsing WorkUnit: ") + FPaths::GetCleanFilename(WwuToProcess);
		SlowTask.EnterProgressFrame(1.0f, FText::FromString(Message));
		FDateTime LastModifiedTime = IFileManager::Get().GetTimeStamp(*WwuToProcess);
		parseWorkUnitFile(WwuToProcess, FString(), ItemType, true, false);
		FDateTime& Time = LastPopTimeMap.FindOrAdd(WwuToProcess);
		Time = LastModifiedTime;
	}
}

void WwiseWorkUnitParser::parseWorkUnitFile(const FString& WorkUnitPath, const FString& RelativePath, EWwiseItemType::Type ItemType, bool ForceRefresh, bool ForceParse)
{
	FXmlFile workUnitXml(WorkUnitPath);

	if (!workUnitXml.IsValid()) 
	{
		visitor->RegisterError(WorkUnitPath, workUnitXml.GetLastError());
		return;
	}

	bool isStandalone = isStandAloneWwu(workUnitXml, WorkUnitPath, ItemType);

	if (!ForceParse && !isStandalone)
	{
		return;
	}

	FString relativePath;
	if (!isStandalone && !RelativePath.IsEmpty())
	{
		relativePath = RelativePath;
	}
	else
	{
		relativePath = WorkUnitPath;
		relativePath.RemoveFromStart(projectRootFolder);
		relativePath.RemoveFromEnd(TEXT(".wwu"));
	}

	visitor->EnterWorkUnit(WorkUnitPath, relativePath, ItemType, isStandalone, ForceRefresh);
	if (!parseWorkUnitXml(workUnitXml, WorkUnitPath, relativePath, ItemType)) 
	{
		visitor->RegisterError(WorkUnitPath, TEXT("XML was valid, but did not have the expected structure."));
	}
	visitor->ExitWorkUnit(isStandalone);
}

bool WwiseWorkUnitParser::isStandAloneWwu(const FXmlFile& Wwu, const FString& wwuPath, EWwiseItemType::Type ItemType)
{
	if (Wwu.IsValid())
	{
		const FXmlNode* RootNode = Wwu.GetRootNode();
		if (RootNode)
		{
			const FXmlNode* EventsNode = RootNode->FindChildNode(EWwiseItemType::DisplayNames[ItemType]);
			if (EventsNode)
			{
				const FXmlNode* WorkUnitNode = EventsNode->FindChildNode(TEXT("WorkUnit"));
				if (WorkUnitNode)
				{
					FString WorkUnitPersistMode = WorkUnitNode->GetAttribute(TEXT("PersistMode"));
					if (WorkUnitPersistMode == TEXT("Standalone"))
					{
						return true;
					}
					else
					{
						return false;
					}
				}
			}
		}
	}
	visitor->RegisterError(wwuPath, TEXT("XML was valid, but did not have the expected structure."));
	return false;
}

bool WwiseWorkUnitParser::parseWorkUnitXml(const FXmlFile& WorkUnitXml, const FString& WorkUnitPath, const FString& RelativePath, EWwiseItemType::Type ItemType)
{
	if (!WorkUnitXml.IsValid())
	{
		return false;
	}

	const FXmlNode* RootNode = WorkUnitXml.GetRootNode();
	if (!RootNode)
	{
		return false;
	}

	const FXmlNode* ItemNode = RootNode->FindChildNode(EWwiseItemType::DisplayNames[ItemType]);
	if (!ItemNode)
	{
		return false;
	}

	const FXmlNode* WorkUnitNode = ItemNode->FindChildNode(TEXT("WorkUnit"));
	if (!WorkUnitNode || (WorkUnitNode->GetAttribute(TEXT("Name")) != FPaths::GetBaseFilename(WorkUnitPath)))
	{
		return false;
	}

	const FXmlNode* CurrentNode = WorkUnitNode->FindChildNode(TEXT("ChildrenList"));
	if (!CurrentNode)
	{
		return true;
	}

	CurrentNode = CurrentNode->GetFirstChildNode();
	if (!CurrentNode)
	{
		return true;
	}

	parseWorkUnitChildren(CurrentNode, WorkUnitPath, RelativePath, ItemType);
	return true;
}

void WwiseWorkUnitParser::recurse(const FXmlNode* CurrentNode, const FString& WorkUnitPath, const FString& CurrentPath, EWwiseItemType::Type ItemType)
{
	if (const FXmlNode* ChildrenNode = CurrentNode->FindChildNode(TEXT("ChildrenList")))
	{
		if (const FXmlNode* FirstChildNode = ChildrenNode->GetFirstChildNode())
		{
			parseWorkUnitChildren(FirstChildNode, WorkUnitPath, CurrentPath, ItemType);
		}
	}
}

void WwiseWorkUnitParser::parseWorkUnitChildren(const FXmlNode* NodeToParse, const FString& WorkUnitPath, const FString& RelativePath, EWwiseItemType::Type ItemType)
{
	for (const FXmlNode* CurrentNode = NodeToParse; CurrentNode; CurrentNode = CurrentNode->GetNextNode())
	{
		FString CurrentTag = CurrentNode->GetTag();
		FString CurrentName = CurrentNode->GetAttribute(TEXT("Name"));
		FString CurrentPath = FPaths::Combine(RelativePath, CurrentName);
		FString CurrentStringId = CurrentNode->GetAttribute(TEXT("ID"));

		FGuid CurrentId;
		FGuid::ParseExact(CurrentStringId, EGuidFormats::DigitsWithHyphensInBraces, CurrentId);

		if (CurrentTag == TEXT("Event"))
		{
			visitor->EnterEvent(CurrentId, CurrentName, CurrentPath);
		}
		else if (CurrentTag == TEXT("SoundBank"))
		{
			visitor->EnterBank(CurrentId, CurrentName, CurrentPath);
		}
		//else if (CurrentTag == TEXT("AcousticTexture"))
		//{
		//	if (ItemType == EWwiseItemType::Type::AcousticTexture)
		//	{
		//		visitor->EnterAcousticTexture(CurrentId, CurrentName, CurrentPath);
		//	}
		//}
		else if (CurrentTag == TEXT("AuxBus"))
		{
			visitor->EnterAuxBus(CurrentId, CurrentName, CurrentPath);
			recurse(CurrentNode, WorkUnitPath, CurrentPath, ItemType);
			visitor->ExitAuxBus();
		}
		else if (CurrentTag == TEXT("WorkUnit"))
		{
			FString currentWwuPhysicalPath = FPaths::Combine(*FPaths::GetPath(WorkUnitPath), *CurrentName) + ".wwu";
			parseWorkUnitFile(currentWwuPhysicalPath, CurrentPath, ItemType, false, true);
		}
		//else if (CurrentTag == TEXT("SwitchGroup"))
		//{
		//	visitor->EnterSwitchGroup(CurrentId, CurrentName, CurrentPath);
		//	recurse(CurrentNode, WorkUnitPath, CurrentPath, ItemType);
		//	visitor->ExitSwitchGroup();
		//}
		//else if (CurrentTag == TEXT("Switch"))
		//{
		//	visitor->EnterSwitch(CurrentId, CurrentName, CurrentPath);
		//}
		//else if (CurrentTag == TEXT("StateGroup"))
		//{
		//	visitor->EnterStateGroup(CurrentId, CurrentName, CurrentPath);
		//	recurse(CurrentNode, WorkUnitPath, CurrentPath, ItemType);
		//	visitor->ExitStateGroup();
		//}
		//else if (CurrentTag == TEXT("State"))
		//{
		//	visitor->EnterState(CurrentId, CurrentName, CurrentPath);
		//}
		//else if (CurrentTag == TEXT("GameParameter"))
		//{
		//	visitor->EnterGameParameter(CurrentId, CurrentName, CurrentPath);
		//}
		//else if (CurrentTag == TEXT("Trigger"))
		//{
		//	visitor->EnterTrigger(CurrentId, CurrentName, CurrentPath);
		//}
		else if (CurrentTag == TEXT("Folder") || CurrentTag == TEXT("Bus"))
		{
			EWwiseItemType::Type currentItemType = EWwiseItemType::Folder;
			if (CurrentTag == TEXT("Bus"))
			{
				currentItemType = EWwiseItemType::Bus;
			}

			visitor->EnterFolderOrBus(CurrentName, CurrentPath, currentItemType);
			recurse(CurrentNode, WorkUnitPath, CurrentPath, ItemType);
			visitor->ExitFolderOrBus();
		}
	}

	visitor->ExitChildrenList();
}

bool WwiseWorkUnitParser::parseBankObjectRef(const FString& SoundBankInfoFilePath)
{
	FXmlFile SoundBankInfoXml(SoundBankInfoFilePath);
	AkAssetDatabase& AssetDatabase = AkAssetDatabase::Get();

	auto String2Int = [](FString StringId)
	{
		uint64 res = uint64(FCString::Atoi(*StringId));
		return res;
	};

	if (!SoundBankInfoXml.IsValid())
	{
		return false;
	}

	const FXmlNode* RootNode = SoundBankInfoXml.GetRootNode();
	const FString& RootTag = RootNode->GetTag();
	if (!RootNode || RootTag != TEXT("SoundBanksInfo"))
	{
		return true;
	}

	const FXmlNode* SoundBanksNode = RootNode->FindChildNode(TEXT("SoundBanks"));
	if (!SoundBanksNode)
	{
		return true;
	}
	AssetDatabase.BankToEventsMap.Empty();

	const TArray<FXmlNode*>& ChildrenNodes = SoundBanksNode->GetChildrenNodes();

	const FXmlNode* BankNode = SoundBanksNode->GetFirstChildNode();
	while (BankNode != nullptr)
	{
		if (BankNode->GetTag() == TEXT("SoundBank"))
		{
			const FXmlNode* NameNode = BankNode->FindChildNode(TEXT("ShortName"));

			const FXmlNode* EventsNode = BankNode->FindChildNode(TEXT("IncludedEvents"));
			if (EventsNode != nullptr && NameNode != nullptr)
			{
				TArray<FString>& EventsArray = AssetDatabase.BankToEventsMap.FindOrAdd(NameNode->GetContent());
				const TArray<FXmlNode*>& Events = EventsNode->GetChildrenNodes();

				for (int i = 0; i < Events.Num(); i++)
				{
					if (Events[i]->GetTag() == TEXT("Event"))
					{
						FString EventName = Events[i]->GetAttribute(TEXT("Name"));
						EventsArray.Add(EventName);
					}
				}
			}
		}
		BankNode = BankNode->GetNextNode();
	}

	return true;
}


#undef LOCTEXT_NAMESPACE