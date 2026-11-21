set_project("artseek")
set_version("0.1.0")
set_languages("c++20")

add_rules("mode.debug", "mode.release")

target("artseek")
    set_kind("binary")
    set_warnings("all")
    add_rules("utils.bin2obj", {extensions = {".dex"}})
    add_includedirs("native/src")
    add_files("native/src/*.cpp")
    add_files("assets/sentinel.dex")
    if is_plat("android") then
        set_runtimes("c++_static")
    end
    if is_mode("release") then
        set_optimize("fastest")
        set_strip("all")
    end
