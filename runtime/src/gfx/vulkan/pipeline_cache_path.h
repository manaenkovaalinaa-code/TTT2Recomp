#pragma once
#include <array>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
namespace gfxvk {
inline std::string device_pipeline_cache_filename(uint32_t vendor,uint32_t device,
    const std::array<uint8_t,16>& uuid) {
  char prefix[32];std::snprintf(prefix,sizeof prefix,"vulkan-%08x-%08x-",vendor,device);
  std::string name(prefix);
  constexpr char hex[]="0123456789abcdef";
  for(auto byte:uuid) { name+=hex[byte>>4];name+=hex[byte&15]; }
  return name+".bin";
}
// A present empty result disables caching; nullopt leaves existing defaults.
// Explicit file overrides always win over directory selection, including "0".
inline std::optional<std::string> pipeline_cache_override(
    const std::optional<std::string>& file,const std::optional<std::string>& directory,
    uint32_t vendor,uint32_t device,const std::array<uint8_t,16>& uuid) {
  if(file) return *file=="0" ? std::string{} : *file;
  if(!directory) return std::nullopt;
  if(directory->empty()||*directory=="0") return std::string{};
  std::string path=*directory;
  if(path.back()!='/'&&path.back()!='\\')path+='/';
  return path+device_pipeline_cache_filename(vendor,device,uuid);
}
}
