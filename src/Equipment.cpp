#include "Equipment.h"
#include "RuntimeRenderer.h"
#include <deque>

namespace ER {
namespace {
using Feature=RE::BSShaderMaterial::Feature;
using TextureSlot=RE::BSTextureSet::Texture;
struct Part { RE::FormID item{}; std::string itemName,name,source; RE::BSGeometry* geometry{}; RE::BSLightingShaderProperty* property{}; };
enum class Action {Apply,Reset,ResetAll};
struct Command {Action action;RE::FormID item{};std::string source;Color color;};
std::mutex mutex;
View view;
std::vector<SavedColor> recipes;
std::deque<Command> commands;
std::atomic<bool> active{false},pending{false};
std::atomic<std::uint64_t> epoch{1};
std::shared_ptr<RenderBindings> current;
constexpr std::size_t maxRecipes=256;
std::string Normalize(std::string s) {
    std::replace(s.begin(),s.end(),'\\','/');
    std::transform(s.begin(),s.end(),s.begin(),[](unsigned char c){return char(std::tolower(c));});
    if(s.starts_with("data/")) s.erase(0,5);
    if(!s.starts_with("textures/")) s.insert(0,"textures/");
    return s;
}
bool ValidSource(const std::string& s) {
    return !s.empty() && s.size()<=1024 && s.starts_with("textures/") && s.ends_with(".dds") &&
        s.find("..")==std::string::npos && s.find(':')==std::string::npos && s.find('\0')==std::string::npos;
}
bool Supported(RE::BSLightingShaderProperty* p) {
    if(!p || !p->material || p->material->GetType()!=RE::BSShaderMaterial::Type::kLighting) return false;
    switch(p->material->GetFeature()) {
    case Feature::kDefault:case Feature::kEnvironmentMap:case Feature::kGlowMap:case Feature::kParallax:
    case Feature::kParallaxOcc:case Feature::kMultilayerParallax:return true;
    default:return false;
    }
}
std::string Source(RE::BSLightingShaderProperty* p) {
    auto* m=static_cast<RE::BSLightingShaderMaterialBase*>(p->material);
    auto set=m->GetTextureSet();const char* path=set?set->GetTexturePath(TextureSlot::kDiffuse):nullptr;
    if((!path || !*path) && m->diffuseTexture) path=m->diffuseTexture->name.c_str();
    return path && *path?Normalize(path):std::string{};
}
void Visit(RE::NiAVObject* obj,RE::TESForm* item,std::vector<Part>& parts,std::set<RE::BSGeometry*>& seen,unsigned depth=0) {
    if(!obj || depth>64 || parts.size()>=1024) return;
    if(auto* geo=obj->AsGeometry();geo && seen.insert(geo).second) {
        auto* property=geo->lightingShaderProp_cast();
        if(Supported(property)) {
            auto source=Source(property);
            if(ValidSource(source)) {
                const char* name=item->GetName();
                parts.push_back({item->GetFormID(),name && *name?name:"Unnamed equipment",obj->name.c_str(),std::move(source),geo,property});
            }
        }
    }
    if(auto* node=obj->AsNode()) for(auto& child:node->GetChildren()) Visit(child.get(),item,parts,seen,depth+1);
}
std::vector<Part> Discover() {
    std::vector<Part> parts;
    auto* player=RE::PlayerCharacter::GetSingleton();if(!player || !player->Is3DLoaded()) return parts;
    std::set<RE::BSGeometry*> seen;auto* skin=player->GetSkin();
    for(bool first:{false,true}) {
        const auto& biped=player->GetBiped(first);if(!biped) continue;
        for(auto& object:biped->objects) {
            auto* item=object.item;
            if(!item || item==skin || !object.partClone || (!item->As<RE::TESObjectARMO>() && !item->As<RE::TESObjectWEAP>())) continue;
            Visit(object.partClone.get(),item,parts,seen);
        }
    }
    return parts;
}
void Tick() {
    struct Guard {~Guard(){pending=false;}} guard;
    static std::uint64_t observed=0;
    const auto session=epoch.load();
    if(observed!=session) {current.reset();PublishBindings({});observed=session;}
    if(!active.load()) return;
    if(auto* ui=RE::UI::GetSingleton();ui && ui->IsMenuOpen(RE::MainMenu::MENU_NAME)) {current.reset();PublishBindings({});return;}
    const auto parts=Discover();
    std::vector<SavedColor> colors;
    {
        std::lock_guard lock(mutex);
        for(const auto& command:commands) {
            ++view.revision;
            if(command.action==Action::ResetAll) {recipes.clear();view.status="All equipment colors reset.";continue;}
            if(command.action==Action::Reset) {
                std::erase_if(recipes,[&](const SavedColor& r){return r.item==command.item && (command.source.empty() || r.source==command.source);});
                view.status="Selected equipment colors reset.";continue;
            }
            std::set<std::string> sources;
            for(const auto& part:parts) if(part.item==command.item && (command.source.empty() || command.source==part.source)) sources.insert(part.source);
            bool full=false;
            for(const auto& source:sources) {
                auto it=std::find_if(recipes.begin(),recipes.end(),[&](const SavedColor& r){return r.item==command.item && r.source==source;});
                SavedColor value{command.item,source,command.color};
                if(it!=recipes.end()) *it=value;
                else if(recipes.size()<maxRecipes) recipes.push_back(value);
                else full=true;
            }
            view.status=sources.empty()?"Item is no longer equipped. Select an equipped item.":full?"Saved-section limit reached. Reset unused item colors.":"Color set. Waiting for equipment to be drawn; close F1 to inspect.";
        }
        commands.clear();colors=recipes;view.colors=recipes;view.recipeCount=recipes.size();view.busy=false;
    }
    auto next=std::make_shared<RenderBindings>();
    for(const auto& part:parts) {
        auto recipe=std::find_if(colors.begin(),colors.end(),[&](const SavedColor& r){return r.item==part.item && r.source==part.source;});
        if(recipe==colors.end() || recipe->color.strength==0) continue;
        RenderBinding binding{RE::NiPointer<RE::BSGeometry>{part.geometry},RE::NiPointer<RE::BSShaderProperty>{part.property},recipe->color};
        if(current) {
            auto old=current->find(part.geometry);
            if(old!=current->end() && old->second.property.get()==part.property && old->second.color==recipe->color) binding.feedback=old->second.feedback;
        }
        next->emplace(part.geometry,std::move(binding));
    }
    unsigned rendered=0,failed=0;
    for(const auto& [_,binding]:*next) {
        if(binding.feedback->passes.load()>0) ++rendered;
        if(binding.feedback->unsupported.load()) ++failed;
    }
    if(session!=epoch.load() || !active.load()) return;
    current=next;PublishBindings(next);
    std::vector<Item> items;
    for(const auto& p:parts) {
        auto it=std::find_if(items.begin(),items.end(),[&](const Item& i){return i.id==p.item;});
        if(it==items.end()) {items.push_back({p.item,p.itemName,{}});it=std::prev(items.end());}
        if(std::none_of(it->surfaces.begin(),it->surfaces.end(),[&](const Surface& s){return s.texture==p.source;})) it->surfaces.push_back({p.name.empty()?"Surface":p.name,p.source});
    }
    std::sort(items.begin(),items.end(),[](const Item& a,const Item& b){return a.name<b.name;});
    {
        std::lock_guard lock(mutex);view.items=std::move(items);
        if(failed) view.status=std::format("{} mesh(es) rendered with color; {} encountered unsupported shader passes. See EquipmentColorRuntime.log.",rendered,failed);
        else if(rendered) view.status=std::format("Applied: {} equipped mesh(es) reached the recolor renderer.",rendered);
    }
}
constexpr std::uint32_t tag=0x45524354,recordTag=0x434F4C52;
void Save(SKSE::SerializationInterface* ser) {
    std::vector<SavedColor> colors;{std::lock_guard lock(mutex);colors=recipes;}
    for(const auto& r:colors) {
        auto length=static_cast<std::uint32_t>(r.source.size());
        if(!ser->OpenRecord(recordTag,1) || !ser->WriteRecordData(r.item) || !ser->WriteRecordData(r.color.rgb) || !ser->WriteRecordData(r.color.strength) || !ser->WriteRecordData(r.color.brightness) || !ser->WriteRecordData(length) || !ser->WriteRecordData(r.source.data(),length)) {logger::error("Could not save colors.");break;}
    }
}
void Load(SKSE::SerializationInterface* ser) {
    std::vector<SavedColor> colors;std::uint32_t type{},version{},size{};
    while(ser->GetNextRecordInfo(type,version,size)) {
        if(type!=recordTag || version!=1 || size<28 || size>1052 || colors.size()>=maxRecipes) continue;
        SavedColor r;std::uint32_t length{};
        if(ser->ReadRecordData(r.item)!=sizeof(r.item) || ser->ReadRecordData(r.color.rgb)!=sizeof(r.color.rgb) || ser->ReadRecordData(r.color.strength)!=sizeof(float) || ser->ReadRecordData(r.color.brightness)!=sizeof(float) || ser->ReadRecordData(length)!=sizeof(length) || !length || length>1024 || size!=28+length) continue;
        r.source.resize(length);
        if(ser->ReadRecordData(r.source.data(),length)!=length || !ValidSource(r.source) || !ValidColor(r.color) || !ser->ResolveFormID(r.item,r.item)) continue;
        colors.push_back(std::move(r));
    }
    logger::info("Loaded {} equipment color settings.",colors.size());
    std::lock_guard lock(mutex);recipes=std::move(colors);view.colors=recipes;++view.revision;
}
void Revert(SKSE::SerializationInterface*) {
    active=false;++epoch;PublishBindings({});
    std::lock_guard lock(mutex);recipes.clear();commands.clear();const auto revision=view.revision+1;view=View{};view.revision=revision;
}
void Queue(Command command) {
    if(!active.load()) return;
    {
        std::lock_guard lock(mutex);
        if(commands.size()>=32) {view.status="Too many pending actions; wait a moment.";return;}
        logger::info("Queued action {} for item {:08X}, section {}",static_cast<int>(command.action),command.item,command.source.empty()?"whole item":command.source);
        commands.push_back(std::move(command));view.busy=true;view.status="Processing equipment color action...";
    }
    Refresh();
}
}
View GetView() {std::lock_guard lock(mutex);return view;}
void Refresh() {
    if(pending.exchange(true)) return;
    SKSE::GetTaskInterface()->AddTask([]{try {Tick();} catch(const std::exception& e) {pending=false;logger::error("Equipment update: {}",e.what());}});
}
void Apply(RE::FormID item,std::string texture,Color color) {
    if(!ValidColor(color)) return;
    if(!RendererReady()) {std::lock_guard lock(mutex);view.status=RendererStatus();return;}
    Queue({Action::Apply,item,std::move(texture),color});
}
void Reset(RE::FormID item,std::string texture) {Queue({Action::Reset,item,std::move(texture)});}
void ResetAll() {Queue({Action::ResetAll});}
void SetSession(bool value) {active=value;++epoch;PublishBindings({});if(value) Refresh();}
void RegisterSerialization() {
    auto* ser=SKSE::GetSerializationInterface();ser->SetUniqueID(tag);ser->SetSaveCallback(Save);ser->SetLoadCallback(Load);ser->SetRevertCallback(Revert);
}
void Start() {
    static std::once_flag once;
    std::call_once(once,[]{
        // This thread only schedules game-thread discovery; it performs no texture or file work.
        static std::jthread pump([](std::stop_token stop){while(!stop.stop_requested()) {if(active.load()) Refresh();std::this_thread::sleep_for(250ms);}});
        logger::info("Equipment update scheduler started (250 ms).");
    });
}
}
