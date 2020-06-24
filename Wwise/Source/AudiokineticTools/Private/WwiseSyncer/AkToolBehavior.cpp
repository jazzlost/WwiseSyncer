#include "AkToolBehavior.h"

#include "AkUnrealHelper.h"
//#include "AkEventBasedToolBehavior.h"
#include "AkLegacyToolBehavior.h"

AkToolBehavior* AkToolBehavior::s_Instance = nullptr;

AkToolBehavior* AkToolBehavior::Get()
{
	if (!s_Instance)
	{
		//if (AkUnrealHelper::IsUsingEventBased())
		//{
		//	s_Instance = new AkEventBasedToolBehavior;
		//}
		//else
		//{
			s_Instance = new AkLegacyToolBehavior;
		//}
	}

	return s_Instance;
}

bool AkToolBehavior::AkAssetManagementManager_ModifyProjectSettings(FString& ProjectContent)
{
	static const auto PropertyListStart = TEXT("<PropertyList>");
	static const TCHAR* PropertiesToAdd[] = {
		TEXT("<Property Name=\"GenerateMultipleBanks\" Type=\"bool\" Value=\"True\"/>"),
		TEXT("<Property Name=\"GenerateSoundBankJSON\" Type=\"bool\" Value=\"True\"/>"),
		TEXT("<Property Name=\"SoundBankGenerateEstimatedDuration\" Type=\"bool\" Value=\"True\"/>"),
		TEXT("<Property Name=\"SoundBankGenerateMaxAttenuationInfo\" Type=\"bool\" Value=\"True\"/>"),
		TEXT("<Property Name=\"SoundBankGeneratePrintGUID\" Type=\"bool\" Value=\"True\"/>"),
	};

	bool modified = false;

	int32 propertyListPosition = ProjectContent.Find(PropertyListStart);
	if (propertyListPosition != -1)
	{
		int32 insertPosition = propertyListPosition + FCString::Strlen(PropertyListStart);

		for (auto itemToAdd : PropertiesToAdd)
		{
			if (ProjectContent.Find(itemToAdd) == -1)
			{
				ProjectContent.InsertAt(insertPosition, FString::Printf(TEXT("\n%s"), itemToAdd));
				modified = true;
			}
		}
	}

	return modified;
}