#ifndef _INCLUDE_LUCKYCHICKEN_PLUGIN_H_
#define _INCLUDE_LUCKYCHICKEN_PLUGIN_H_

#include <ISmmPlugin.h>
#include <sh_vector.h>
#include "utlvector.h"
#include "ehandle.h"
#include <iserver.h>
#include <eiface.h>
#include <entity2/entitysystem.h>
#include "igameevents.h"
#include "vector.h"
#include <deque>
#include <functional>
#include <utlstring.h>
#include <KeyValues.h>
#include "CCSPlayerController.h"
#include "CCSPlayerPawn.h"
#include "CChicken.h"
#include "entitykeyvalues.h"
#include "include/menus.h"
#include "include/shop.h"
#include "include/sql_mm.h"
#include "include/mysql_mm.h"

class LuckyChicken final : public ISmmPlugin, public IMetamodListener
{
public:
    bool Load(PluginId id, ISmmAPI* ismm, char* error, size_t maxlen, bool late);
    bool Unload(char* error, size_t maxlen);
    void AllPluginsLoaded();
private:
    const char* GetAuthor();
    const char* GetName();
    const char* GetDescription();
    const char* GetURL();
    const char* GetLicense();
    const char* GetVersion();
    const char* GetDate();
    const char* GetLogTag();
};

#endif // _INCLUDE_LUCKYCHICKEN_PLUGIN_H_
