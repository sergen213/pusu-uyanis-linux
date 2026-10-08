#include "game.hpp"
#include "resources.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <stdexcept>

namespace pusu {
namespace {
struct Chapter { std::string_view level, label; };
// Original table 00486480, including the legacy single-byte Turkish font encoding.
constexpr std::array<Chapter,13> chapters{{
    {"surlar_1","Baba Evi"},{"surlar_2","Harabe"},{"surlar_3","Sur i\xe7i"},
    {"surlar_4","Mahzen"},{"laboratuar","Sanayi Sitesi"},
    {"haydarpasa_limani","Haydarpa\xfe" "a"},{"magara_1","Esrarengiz Liman"},
    {"magara_2","Asma K\xf6pr\xfc"},{"askeri_us_1","Laboratuvar"},
    {"askeri_us_2","G\xfc\xe7 Santrali"},{"askeri_us_3","Denek Ko\xf0u\xfeu"},
    {"askeri_us_4","Havaland\xfd" "rma Dairesi"},{"askeri_us_5","Komuta Merkezi"}
}};
struct SavedFile {
    std::filesystem::path path;
    std::string name;
    std::filesystem::file_time_type modified;
};
char lower(char c) { return c>='A'&&c<='Z'?static_cast<char>(c+'a'-'A'):c; }
bool equal_name(std::string_view a,std::string_view b) {
    return a.size()==b.size()&&std::equal(a.begin(),a.end(),b.begin(),
        [](char x,char y){return lower(x)==lower(y);});
}
bool starts_name(std::string_view name,std::string_view prefix) {
    return name.size()>=prefix.size()&&equal_name(name.substr(0,prefix.size()),prefix);
}
std::vector<SavedFile> saved_files() {
    const auto directory=user_data_directory()/"save";
    std::vector<SavedFile> result;
    if(!std::filesystem::exists(directory))return result;
    for(const auto& entry:std::filesystem::directory_iterator(directory)) {
        if(!entry.is_regular_file()||!equal_name(entry.path().extension().string(),".psv"))continue;
        auto name=entry.path().stem().string();
        if(name.empty()||name.front()=='_'||equal_name(name,"player"))continue;
        result.push_back({entry.path(),std::move(name),entry.last_write_time()});
    }
    // 00432300 sorts native last-write metadata; latest files are visited first.
    std::stable_sort(result.begin(),result.end(),
        [](const SavedFile& a,const SavedFile& b){return a.modified>b.modified;});
    return result;
}
int saved_chapter(std::string_view name) {
    const auto separator=name.rfind('_');
    if(separator==name.npos)return -1;
    const auto base=name.substr(0,separator);
    for(std::size_t i=0;i<chapters.size();++i)
        if(equal_name(base,chapters[i].level))return static_cast<int>(i);
    return -1;
}
int checkpoint_number(std::string_view name) {
    const auto separator=name.rfind('_');
    if(separator==name.npos)return 0;
    int value{};
    const auto tail=name.substr(separator+1);
    const auto parsed=std::from_chars(tail.data(),tail.data()+tail.size(),value);
    return parsed.ec==std::errc{}?static_cast<unsigned char>(value):0;
}
}

void Game::refresh_load_menu() {
    // 0045df20 unlocks the prefix through the highest chapter with a save.
    const auto files=saved_files();
    int maximum=0,latest=-1;
    for(const auto& file:files) {
        const int chapter=saved_chapter(file.name);
        if(chapter<0)continue;
        maximum=std::max(maximum,chapter);
        if(latest<0)latest=chapter;
    }
    std::vector<std::string> labels;
    menu_chapters_.clear();
    for(int i=0;i<=maximum;++i) {
        menu_chapters_.push_back(i);
        labels.emplace_back(chapters[static_cast<std::size_t>(i)].label);
    }
    interface_.set_list_items("oyun_yukle","list_oyun_yukle_bolum",labels);
    if(latest>=0)interface_.set_selected_item("oyun_yukle","list_oyun_yukle_bolum",latest);
    select_load_chapter();
}

void Game::select_load_chapter() {
    const int selected=interface_.selected_item("oyun_yukle","list_oyun_yukle_bolum");
    if(selected<0||static_cast<std::size_t>(selected)>=menu_chapters_.size())return;
    const auto& chapter=chapters[static_cast<std::size_t>(menu_chapters_[static_cast<std::size_t>(selected)])];
    interface_.set_control_shader("picture_oyun_yukle_bolum",chapter.level);
    const auto files=saved_files();
    std::size_t count{};
    int latest=0;
    for(const auto& file:files)if(starts_name(file.name,chapter.level)) {
        if(!count)latest=checkpoint_number(file.name);
        ++count;
    }
    if(count>255)throw std::runtime_error("Original checkpoint list exceeds its byte-sized capacity");
    menu_save_slots_.clear();
    std::vector<std::string> labels;
    for(std::size_t i=0;i<count;++i) {
        // 0045dc30/0045bf10 use ordinal slots, not filename suffixes as row IDs.
        const auto name=std::string(chapter.level)+"_"+std::to_string(i);
        auto label="Kayit_"+std::to_string(i+1);
        menu_save_slots_.push_back({user_data_directory()/"save"/(name+".psv"),name,label,"save_"+name});
        labels.push_back(std::move(label));
    }
    interface_.set_save_slots(labels);
    if(count)interface_.set_selected_item("oyun_yukle","list_oyun_yukle_checkpoint",
        std::min(latest,static_cast<int>(count)-1));
    interface_.set_control_enabled("list_oyun_yukle_checkpoint",count!=0);
    interface_.set_control_enabled("oyun_yukle_yukle",count!=0);
    select_load_checkpoint();
}

void Game::select_load_checkpoint() {
    const int selected=interface_.selected_item("oyun_yukle","list_oyun_yukle_checkpoint");
    const auto shader=selected<0||static_cast<std::size_t>(selected)>=menu_save_slots_.size()
        ?std::string_view("menu_no_checkpoint")
        :std::string_view(menu_save_slots_[static_cast<std::size_t>(selected)].shader);
    interface_.set_control_shader("picture_oyun_yukle_checkpoint",shader);
}


void Game::boot() {
    register_hosts();
    load_language();
    media_.set_random_source(this,[](void* context) {
        return static_cast<Game*>(context)->actors_.random_word();
    });
    interface_.set_game_active(false);
    menu_=true;
    interface_.show_menu("ana_sayfa");
}

void Game::new_game() {
    // 004587b0 copies config+0x110's string (00452120), default surlar_1.
    // The outer update owns the load; a menu callback must not replace the VM.
    menu_=false;
    interface_.hide_menu();
    queue_level("surlar_1",true);
}
void Game::action(const MenuAction& action) {
    const auto& name=action.name;
    if(name.ends_with("_on_activate")) {
        menu_=true;
        interface_.set_game_active(level_!=nullptr);
        if(name=="oyun_yukle_on_activate")refresh_load_menu();
        else if(name=="ana_sayfa_on_activate")
            interface_.set_control_enabled("oyuna_devam",level_!=nullptr||!saved_files().empty());
    } else if(name=="oyuna_devam") {
        if(!level_) {
            const auto files=saved_files();
            if(files.empty())return;
            load_checkpoint(files.front().path);
        }
        menu_=false;
        interface_.hide_menu();
    } else if(name=="yeni_oyun") {
        if(level_)interface_.show_menu("yeni_oyun_onay");
        else new_game();
    } else if(name=="yeni_oyun_onay_evet") {
        new_game();
    } else if(name=="cikis_onay_evet") {
        quit_=true;
    } else if(name=="list_oyun_yukle_bolum") {
        select_load_chapter();
    } else if(name=="list_oyun_yukle_checkpoint") {
        select_load_checkpoint();
    } else if(name=="oyun_yukle_yukle") {
        const int selected=interface_.selected_item("oyun_yukle","list_oyun_yukle_checkpoint");
        if(selected<0 || static_cast<std::size_t>(selected)>=menu_save_slots_.size())return;
        // Checkpoint restore stages original continuation defaults before commit.
        const auto path=menu_save_slots_[static_cast<std::size_t>(selected)].path;
        load_checkpoint(path);
        menu_=false;
        interface_.hide_menu();
    }
}


} // namespace pusu
