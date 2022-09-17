#include "plugin.hpp"
#include <exception>
#include <obs-module.h>
#include <obs.h>

const char *RENDER_MODE = "RenderMode";
const char *MODE_BLEND = "RenderModeBlend";
const char *MODE_MASK = "RenderModeMask";

enum render_mode_t {
  RENDER_MODE_BLEND,
  RENDER_MODE_MASK,
};

struct render_filter_data {
  obs_source_t *self;
  obs_source_t *parent;
  gs_effect_t *effect;
  gs_effect_t *effect2;
  gs_texture_t *texture;
  gs_texture_t *texture2;
  gs_eparam_t *mask_param;
  gs_eparam_t *mask_param2;
  uint8_t *mask_buffer;
  uint8_t *mask_buffer2;
  uint32_t mask_width;
  uint32_t mask_height;
  render_mode_t render_mode;
  int cnt;
};

const char *render_get_name(void *data) {
  UNUSED_PARAMETER(data);
  return obs_module_text("VirtualBackGroundRenderFilter");
}

void render_destroy(void *data) {
  render_filter_data *filter_data = static_cast<render_filter_data *>(data);
  if (filter_data) {
    obs_enter_graphics();
    gs_effect_destroy(filter_data->effect);
    gs_effect_destroy(filter_data->effect2);
    gs_texture_destroy(filter_data->texture);
    gs_texture_destroy(filter_data->texture2);
    obs_leave_graphics();
    bfree(filter_data->mask_buffer);
    bfree(filter_data->mask_buffer2);
    bfree(filter_data);
  }
}

void destroy_texture(render_filter_data *filter_data) {
  obs_enter_graphics();
  if (filter_data->texture) {
    gs_texture_destroy(filter_data->texture);
    filter_data->texture = NULL;
  }
  if (filter_data->texture2) {
    gs_texture_destroy(filter_data->texture2);
    filter_data->texture2 = NULL;
  }
  obs_leave_graphics();
}

void set_texture(render_filter_data *filter_data) {
  uint32_t width = get_mask_width(filter_data->parent);
  uint32_t height = get_mask_height(filter_data->parent);
  if (width == 0 || height == 0) {
    return;
  }

  if (filter_data->texture || filter_data->texture2) {
    destroy_texture(filter_data);
  }
  obs_enter_graphics();
  filter_data->texture = gs_texture_create(width, height, GS_A8, 1, NULL, GS_DYNAMIC);
  if (filter_data->texture == NULL) {
    blog(LOG_ERROR, "[Virtual BG renderer] Can't create A8 texture.");
  }
  filter_data->texture2 = gs_texture_create(width, height, GS_RGBA, 1, NULL, GS_DYNAMIC);
  if (filter_data->texture2 == NULL) {
    blog(LOG_ERROR, "[Virtual BG renderer] Can't create RGBA texture.");
  }
  obs_leave_graphics();
}

void render_update(void *data, obs_data_t *settings) {
  render_filter_data *filter_data = static_cast<render_filter_data *>(data);

  const char *render_mode = obs_data_get_string(settings, RENDER_MODE);
  if (strcmp(render_mode, MODE_BLEND) == 0) {
    filter_data->render_mode = RENDER_MODE_BLEND;
  } else {
    filter_data->render_mode = RENDER_MODE_MASK;
  }
}

void *render_create(obs_data_t *settings, obs_source_t *source) {
  UNUSED_PARAMETER(settings);
  blog(LOG_INFO, "[Virtual BG renderer] render_create version=v%s", VBG_VERSION);

  obs_enter_graphics();

  struct render_filter_data *filter_data =
      reinterpret_cast<render_filter_data *>(bzalloc(sizeof(struct render_filter_data)));
  filter_data->self = source;
  auto effect_file = obs_module_file("virtualbg.effect");
  filter_data->effect = gs_effect_create_from_file(effect_file, NULL);
  bfree(effect_file);
  if (!filter_data->effect) {
    render_destroy(filter_data);
    filter_data = NULL;
    return NULL;
  }
  filter_data->mask_param = gs_effect_get_param_by_name(filter_data->effect, "mask");

  effect_file = obs_module_file("virtualbg-mask.effect");
  filter_data->effect2 = gs_effect_create_from_file(effect_file, NULL);
  bfree(effect_file);
  if (!filter_data->effect2) {
    render_destroy(filter_data);
    filter_data = NULL;
    return NULL;
  }
  filter_data->mask_param2 = gs_effect_get_param_by_name(filter_data->effect2, "mask");

  obs_leave_graphics();

  render_update(filter_data, settings);

  return filter_data;
}

void render_defaults(obs_data_t *settings) { obs_data_set_default_string(settings, RENDER_MODE, MODE_BLEND); }

obs_properties_t *render_properties(void *data) {
  UNUSED_PARAMETER(data);
  obs_properties_t *ppts = obs_properties_create();
  obs_property_t *p_mode = obs_properties_add_list(ppts, RENDER_MODE, obs_module_text(RENDER_MODE),
                                                   OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
  obs_property_list_add_string(p_mode, obs_module_text(MODE_BLEND), MODE_BLEND);
  obs_property_list_add_string(p_mode, obs_module_text(MODE_MASK), MODE_MASK);
  return ppts;
}

void render_video_render(void *data, gs_effect_t *effect) {
  try {
    render_filter_data *filter_data = static_cast<render_filter_data *>(data);
    if (filter_data == NULL || filter_data->effect == NULL || filter_data->effect2 == NULL) {
      return;
    }
    if (filter_data->parent == NULL) {
      filter_data->parent = obs_filter_get_parent(filter_data->self);
      if (filter_data->parent == NULL) {
        return;
      }
    }
    uint32_t width = (uint32_t)get_mask_width(filter_data->parent);
    uint32_t height = (uint32_t)get_mask_height(filter_data->parent);

    if (filter_data->mask_width != width || filter_data->mask_height != height) {
      if (filter_data->texture != NULL || filter_data->texture2 != NULL) {
        destroy_texture(filter_data);
      }
      if (filter_data->mask_buffer) {
        bfree(filter_data->mask_buffer);
        filter_data->mask_buffer = NULL;
      }
      filter_data->mask_width = width;
      filter_data->mask_height = height;
    }

    if (filter_data->texture == NULL || filter_data->texture2 == NULL) {
      set_texture(filter_data);
    }

    if (!filter_data->mask_buffer) {
      filter_data->mask_buffer = (uint8_t *)bmalloc(sizeof(uint8_t) * width * height);
    }
    get_mask_data(filter_data->parent, filter_data->mask_buffer);

    if (filter_data->render_mode == RENDER_MODE_MASK) {
      if (!filter_data->mask_buffer2) {
        filter_data->mask_buffer2 = (uint8_t *)bmalloc(sizeof(uint8_t) * width * height * 4);
      }
      for (int i = 0; i < width * height; ++i) {
        filter_data->mask_buffer2[i * 4 + 0] = filter_data->mask_buffer[i];
        filter_data->mask_buffer2[i * 4 + 1] = filter_data->mask_buffer[i];
        filter_data->mask_buffer2[i * 4 + 2] = filter_data->mask_buffer[i];
        filter_data->mask_buffer2[i * 4 + 3] = 255;
      }
    }

    obs_enter_graphics();
    if (filter_data->render_mode == RENDER_MODE_BLEND) {
      gs_texture_set_image(filter_data->texture, filter_data->mask_buffer, width, false);
    } else if (filter_data->render_mode == RENDER_MODE_MASK) {
      gs_texture_set_image(filter_data->texture2, filter_data->mask_buffer2, width * 4, false);
    } else {
      blog(LOG_ERROR, "[Virtual BG renderer] Unkown render_mode");
      return;
    }
    obs_leave_graphics();

    if (filter_data->render_mode == RENDER_MODE_BLEND) {
      if (!obs_source_process_filter_begin(filter_data->self, GS_RGBA, OBS_ALLOW_DIRECT_RENDERING)) {
        return;
      }
      gs_effect_set_texture(filter_data->mask_param, filter_data->texture);
      obs_source_process_filter_end(filter_data->self, filter_data->effect, 0, 0);
    } else if (filter_data->render_mode == RENDER_MODE_MASK) {
      if (!obs_source_process_filter_begin(filter_data->self, GS_RGBA, OBS_ALLOW_DIRECT_RENDERING)) {
        return;
      }
      gs_effect_set_texture(filter_data->mask_param2, filter_data->texture2);
      obs_source_process_filter_end(filter_data->self, filter_data->effect2, 0, 0);
    } else {
      blog(LOG_ERROR, "[Virtual BG renderer] Unkown render_mode");
      return;
    }

    filter_data->cnt++;
  } catch (const std::exception &ex) {
    blog(LOG_ERROR, "[Virtual BG renderer] exception in render %s", ex.what());
  }
}

struct obs_source_info obs_virtualbg_render_source_info {
  .id = "virtualbg-render", .type = OBS_SOURCE_TYPE_FILTER,
  .output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_SRGB, .get_name = render_get_name, .create = render_create,
  .destroy = render_destroy, .get_defaults = render_defaults, .get_properties = render_properties,
  .update = render_update, .video_render = render_video_render
};
