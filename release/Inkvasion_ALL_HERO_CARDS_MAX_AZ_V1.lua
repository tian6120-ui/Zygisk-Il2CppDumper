-- Inkvasion 1.3.0 / AZ ScriptCore V0.7
-- ALL HERO + ALL FIGHT/BUILD CARDS + TRUE MAX LEVEL
-- Dynamic master / rerun safe / client-local
-- NO HTTP / NO STORE / NO IAP / NO CURRENCY
--
-- Inkvasion standalone one-shot auto supplement
local TAG="[INKVASION ALL HERO+CARDS MAX V1] "
local function log(s) print(TAG..tostring(s)) end
local function fail(s) error(tostring(s),0) end
local function D(d,n,p) return {declaring=d,name=n,params=p or {}} end

local TYPE_GET=D("System.Type","GetType",{"System.String"})
local ACTIVATOR_CREATE=D("System.Activator","CreateInstance",{"System.Type"})
local ARRAY_CREATE=D("System.Array","CreateInstance",{"System.Type","System.Int32"})
local ARRAY_SET=D("System.Array","SetValue",{"System.Object","System.Int32"})
local TYPE_MAKE_GENERIC=D("System.Type","MakeGenericType",{"System.Type[]"})

local function exact(desc,recv,...)
  local ok,v=pcall(Call.exact,desc,recv,...)
  if not ok then return nil,tostring(v) end
  return v,nil
end

local function get_type(name)
  local t,e=exact(TYPE_GET,nil,name)
  if not t then fail("TYPE_NOT_FOUND | "..name.." | "..tostring(e)) end
  return t
end

local function new_type(t)
  local o,e=exact(ACTIVATOR_CREATE,nil,t)
  if not o then fail("CREATE_FAILED | "..tostring(e)) end
  return o
end

local function new_named(name) return new_type(get_type(name)) end

local function new_generic_list(elementType)
  local systemType=get_type("System.Type")
  local listDef=get_type("System.Collections.Generic.List`1, mscorlib")
  local args,e=exact(ARRAY_CREATE,nil,systemType,1)
  if not args then fail("TYPE_ARRAY_CREATE_FAILED | "..tostring(e)) end
  local _,se=exact(ARRAY_SET,args,elementType,0)
  if se then fail("TYPE_ARRAY_SET_FAILED | "..tostring(se)) end
  local closed,ce=exact(TYPE_MAKE_GENERIC,listDef,args)
  if not closed then fail("MAKE_GENERIC_FAILED | "..tostring(ce)) end
  return new_type(closed)
end

local function root(name)
  local c=Class.fromName(name)
  if not c then fail("CLASS_NOT_FOUND | "..name) end
  local xs=c:findObjectsFresh()
  if type(xs)~="table" or not xs[1] then fail("ROOT_NOT_READY | "..name) end
  return xs[1]
end

local function count(list)
  if not list then return -1 end
  local ok,v=pcall(function() return list:get_Count() end)
  if not ok then return -1 end
  return tonumber(v) or -1
end

local function item(list,i)
  local ok,v=pcall(function() return list:get_Item(i) end)
  if not ok then return nil end
  return v
end

local function add(list,v)
  local ok,e=pcall(function() list:Add(v) end)
  if not ok then fail("LIST_ADD_FAILED | "..tostring(e)) end
end

local function collect_master(list)
  local out,set={},{}
  local n=count(list)
  if n<=0 then return out,set end
  for i=0,n-1 do
    local row=item(list,i)
    local id=row and tonumber(row.id) or nil
    if id~=nil and not set[id] then set[id]=true; out[#out+1]=id end
  end
  table.sort(out)
  return out,set
end

local function owned_heroes(list)
  local set={}
  local n=count(list)
  for i=0,n-1 do
    local id=tonumber(item(list,i))
    if id~=nil then set[id]=true end
  end
  return set
end

local function owned_cards(list)
  local set={}
  local n=count(list)
  for i=0,n-1 do
    local row=item(list,i)
    local id=row and tonumber(row.ItemId) or nil
    if id~=nil then set[id]=true end
  end
  return set
end

local function missing(master,owned)
  local r={}
  for _,id in ipairs(master) do if not owned[id] then r[#r+1]=id end end
  return r
end

local function merge(a,b)
  local r,s={},{}
  for _,x in ipairs(a) do if not s[x] then s[x]=true;r[#r+1]=x end end
  for _,x in ipairs(b) do if not s[x] then s[x]=true;r[#r+1]=x end end
  table.sort(r)
  return r
end

local function max_all_cards(list)
  local total,changed,failed,remain=0,0,0,0
  local failLogged=0
  local n=count(list)
  if n<0 then return 0,0,1,1 end

  for i=0,n-1 do
    local row=item(list,i)
    if row then
      total=total+1
      local id=tonumber(row.ItemId) or -1

      local okMax,maxLv=pcall(function() return row:GetShowingMaxLevel() end)
      maxLv=okMax and tonumber(maxLv) or nil

      if maxLv and maxLv>0 then
        local okCur,curLv=pcall(function() return row:GetShowingLevel() end)
        curLv=okCur and tonumber(curLv) or nil

        if curLv and curLv>=maxLv then
          -- already max
        else
          local okSet,setErr=pcall(function() row:SetLevelByShowingLevel(maxLv) end)
          local okVerify,afterLv=pcall(function() return row:GetShowingLevel() end)
          afterLv=okVerify and tonumber(afterLv) or nil

          if okSet and afterLv and afterLv>=maxLv then
            changed=changed+1
          else
            failed=failed+1
            remain=remain+1
            if failLogged<10 then
              log("MAX LEVEL FAIL | id="..tostring(id)..
                  " cur="..tostring(curLv)..
                  " max="..tostring(maxLv)..
                  " after="..tostring(afterLv)..
                  " err="..tostring(setErr))
              failLogged=failLogged+1
            end
          end
        end
      else
        failed=failed+1
        remain=remain+1
        if failLogged<10 then
          log("MAX LEVEL FAIL | id="..tostring(id).." | GetShowingMaxLevel unavailable")
          failLogged=failLogged+1
        end
      end
    end
  end

  return total,changed,failed,remain
end

local function main()
  log("START | dynamic master | one shot")
  log("NO HTTP / NO STORE / NO IAP / NO CURRENCY")

  local payment=root("GamePaymentManager")
  local config=root("GameExternal.ExternalConfigManager")

  local okH,heroMasterList=pcall(function() return config:GetHeroConfigList() end)
  if not okH or count(heroMasterList)<=0 then fail("HERO_MASTER_NOT_READY") end

  local okF,fightList=pcall(function() return payment:get_FightCardItemConfigList() end)
  local okB,buildList=pcall(function() return payment:get_BuildingCardItemConfigList() end)
  if not okF or not okB or count(fightList)<=0 or count(buildList)<=0 then
    pcall(function() payment:LoadConfigs() end)
    okF,fightList=pcall(function() return payment:get_FightCardItemConfigList() end)
    okB,buildList=pcall(function() return payment:get_BuildingCardItemConfigList() end)
  end
  if not okF or count(fightList)<=0 then fail("FIGHT_MASTER_NOT_READY") end
  if not okB or count(buildList)<=0 then fail("BUILD_MASTER_NOT_READY") end

  local okOH,heroList=pcall(function() return payment:get_HeroList() end)
  local okOC,cardList=pcall(function() return payment:get_CardList() end)
  if not okOH or not okOC or count(heroList)<=0 or count(cardList)<=0 then fail("USER_ASSETS_NOT_READY") end

  local heroMaster=collect_master(heroMasterList)
  local fightMaster=collect_master(fightList)
  local buildMaster=collect_master(buildList)
  if #heroMaster==0 or #fightMaster==0 or #buildMaster==0 then fail("MASTER_SCAN_EMPTY") end

  local hOwned=owned_heroes(heroList)
  local cOwned=owned_cards(cardList)
  local missHero=missing(heroMaster,hOwned)
  local missFight=missing(fightMaster,cOwned)
  local missBuild=missing(buildMaster,cOwned)
  local missCards=merge(missFight,missBuild)

  log("MASTER | hero="..#heroMaster.." fight="..#fightMaster.." build="..#buildMaster)
  log("MISSING | hero="..#missHero.." fight="..#missFight.." build="..#missBuild)

  if #missCards>0 then
    local userCardType=get_type("ChillyRoom.Services.BuildingGame.UserCard, Assembly-CSharp")
    local change=new_named("ChillyRoom.Services.BuildingGame.CardChange, Assembly-CSharp")
    local addList=new_generic_list(userCardType)
    local modifyList=new_generic_list(userCardType)

    for _,id in ipairs(missCards) do
      local uc=new_type(userCardType)
      local ok1,e1=pcall(function() uc:set_CardId(id) end)
      local ok2,e2=pcall(function() uc:set_CardLevel(1) end)
      local ok3,e3=pcall(function() uc:set_CardExp(0) end)
      if not ok1 then fail("SET_CARD_ID_FAILED | "..tostring(e1)) end
      if not ok2 then fail("SET_CARD_LEVEL_FAILED | "..tostring(e2)) end
      if not ok3 then fail("SET_CARD_EXP_FAILED | "..tostring(e3)) end
      add(addList,uc)
    end

    local okA,eA=pcall(function() change:set_AddList(addList) end)
    local okM,eM=pcall(function() change:set_ModifyList(modifyList) end)
    if not okA then fail("SET_ADDLIST_FAILED | "..tostring(eA)) end
    if not okM then fail("SET_MODIFYLIST_FAILED | "..tostring(eM)) end
    local okU,eU=pcall(function() payment:UnpackCardChange(change) end)
    if not okU then fail("UNPACK_CARD_FAILED | "..tostring(eU)) end
    log("CARD APPLY | PASS | add="..#missCards)
  end

  if #missHero>0 then
    local okT,heroListType=pcall(function() return heroList:GetType() end)
    if not okT or not heroListType then fail("HERO_LIST_TYPE_FAILED") end
    local adds=new_type(heroListType)
    for _,id in ipairs(missHero) do add(adds,id) end
    local okU,eU=pcall(function() payment:UnpackHeroChange(adds) end)
    if not okU then fail("UNPACK_HERO_FAILED | "..tostring(eU)) end
    log("HERO APPLY | PASS | add="..#missHero)
  end

  local heroAfter=payment:get_HeroList()
  local cardAfter=payment:get_CardList()

  local maxTotal,maxChanged,maxFailed,maxRemain=max_all_cards(cardAfter)
  log("MAX LEVEL | total="..maxTotal.." changed="..maxChanged.." failed="..maxFailed.." remain="..maxRemain)
  if maxRemain~=0 then fail("MAX_LEVEL_VERIFY_FAILED | remain="..tostring(maxRemain)) end

  local h2=owned_heroes(heroAfter)
  local c2=owned_cards(cardAfter)
  local rh=#missing(heroMaster,h2)
  local rf=#missing(fightMaster,c2)
  local rb=#missing(buildMaster,c2)
  log("VERIFY | heroRemain="..rh.." fightRemain="..rf.." buildRemain="..rb)
  if rh~=0 or rf~=0 or rb~=0 then fail("VERIFY_FAILED") end

  log("RESULT | PASS | ALL HERO + FIGHT + BUILD + MAX LEVEL")
end

local ok,err=pcall(main)
if not ok then log("FATAL | "..tostring(err)); error(err,0) end
