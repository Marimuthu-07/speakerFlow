#include <node_api.h>
#include <pipewire/pipewire.h>
#include <pipewire/extensions/metadata.h>
#include <pipewire/impl-module.h>
#include <spa/param/route.h>
#include <spa/param/props.h>
#include <spa/pod/builder.h>
#include <spa/pod/parser.h>
#include <spa/pod/iter.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <math.h>
#include <errno.h>

#define MAX_NODES 1024
#define MAX_LINKS 2048
#define MAX_DEVICES 64
#define MAX_CHANNELS 16
#define MAX_LOADED_MODULES 64

static int parse_int(const char *s, int fallback)
{
    if (!s || !*s) return fallback;

    int sign = 1;
    int value = 0;

    if (*s == '-') {
        sign = -1;
        s++;
    }

    if (!*s) return fallback;

    while (*s >= '0' && *s <= '9') {
        value = value * 10 + (*s - '0');
        s++;
    }

    return sign * value;
}

typedef struct {
    uint32_t id;
    struct pw_device *proxy;
    struct spa_hook listener;
} DeviceItem;

typedef struct {
    uint32_t id;
    struct pw_node *proxy;
    struct spa_hook listener;
    char *name;
    char *description;
    char *media_class;
    char *state;
    bool is_virtual;
    bool is_corked;
    float volume;
    float channel_volumes[MAX_CHANNELS];
    uint32_t n_channels;
    bool mute;
    uint32_t client_id;
    uint32_t device_id;
    int32_t card_profile_device;
    int32_t route_index;
    struct pw_properties *props;
} NodeItem;

typedef struct {
    uint32_t id;
    struct pw_link *proxy;
    struct spa_hook listener;
    uint32_t output_node_id;
    uint32_t input_node_id;
} LinkItem;

typedef struct {
    uint32_t id;
    struct pw_metadata *proxy;
    struct spa_hook listener;
    char default_sink[256];
} MetadataItem;

typedef struct {
    uint32_t id;
    struct pw_impl_module *mod;
} LoadedModuleItem;

typedef struct {
    struct pw_thread_loop *loop;
    struct pw_context *context;
    struct pw_core *core;
    struct pw_registry *registry;
    struct spa_hook registry_listener;
    struct spa_hook core_listener;

    NodeItem *nodes[MAX_NODES];
    uint32_t node_count;

    LinkItem *links[MAX_LINKS];
    uint32_t link_count;

    DeviceItem *devices[MAX_DEVICES];
    uint32_t device_count;

    MetadataItem metadata;
    bool has_metadata;

    LoadedModuleItem modules[MAX_LOADED_MODULES];
    uint32_t module_count;
    uint32_t next_module_id;

    int sync_seq;
    bool sync_done;
    int sync_error;
    int seq_counter;

    napi_threadsafe_function tsfn;
    bool initialized;
} BackendState;

static BackendState g_state;

static void notify_js_event(const char *event_name) {
    if (!g_state.initialized || !g_state.tsfn) return;
    napi_call_threadsafe_function(g_state.tsfn, (void *)event_name, napi_tsfn_nonblocking);
}

static NodeItem *find_node(uint32_t id) {
    for (uint32_t i = 0; i < g_state.node_count; i++) {
        if (g_state.nodes[i] && g_state.nodes[i]->id == id) return g_state.nodes[i];
    }
    return NULL;
}

static DeviceItem *find_device(uint32_t id) {
    for (uint32_t i = 0; i < g_state.device_count; i++) {
        if (g_state.devices[i] && g_state.devices[i]->id == id) return g_state.devices[i];
    }
    return NULL;
}

static void free_node(NodeItem *node) {
    if (!node) return;
    if (node->proxy) {
        spa_hook_remove(&node->listener);
        pw_proxy_destroy((struct pw_proxy *)node->proxy);
        node->proxy = NULL;
    }
    free(node->name);
    free(node->description);
    free(node->media_class);
    free(node->state);
    if (node->props) {
        pw_properties_free(node->props);
        node->props = NULL;
    }
    free(node);
}

static void remove_node(uint32_t id) {
    for (uint32_t i = 0; i < g_state.node_count; i++) {
        if (g_state.nodes[i] && g_state.nodes[i]->id == id) {
            NodeItem *to_free = g_state.nodes[i];
            if (i < g_state.node_count - 1) {
                g_state.nodes[i] = g_state.nodes[g_state.node_count - 1];
                g_state.nodes[g_state.node_count - 1] = NULL;
            } else {
                g_state.nodes[i] = NULL;
            }
            g_state.node_count--;
            free_node(to_free);
            notify_js_event("devices-changed");
            notify_js_event("streams-changed");
            break;
        }
    }
}

static void free_link(LinkItem *link) {
    if (!link) return;
    if (link->proxy) {
        spa_hook_remove(&link->listener);
        pw_proxy_destroy((struct pw_proxy *)link->proxy);
        link->proxy = NULL;
    }
    free(link);
}

static void remove_link(uint32_t id) {
    for (uint32_t i = 0; i < g_state.link_count; i++) {
        if (g_state.links[i] && g_state.links[i]->id == id) {
            LinkItem *to_free = g_state.links[i];
            if (i < g_state.link_count - 1) {
                g_state.links[i] = g_state.links[g_state.link_count - 1];
                g_state.links[g_state.link_count - 1] = NULL;
            } else {
                g_state.links[i] = NULL;
            }
            g_state.link_count--;
            free_link(to_free);
            notify_js_event("streams-changed");
            break;
        }
    }
}

static void free_device(DeviceItem *dev) {
    if (!dev) return;
    if (dev->proxy) {
        spa_hook_remove(&dev->listener);
        pw_proxy_destroy((struct pw_proxy *)dev->proxy);
        dev->proxy = NULL;
    }
    free(dev);
}

static void remove_device(uint32_t id) {
    for (uint32_t i = 0; i < g_state.device_count; i++) {
        if (g_state.devices[i] && g_state.devices[i]->id == id) {
            DeviceItem *to_free = g_state.devices[i];
            if (i < g_state.device_count - 1) {
                g_state.devices[i] = g_state.devices[g_state.device_count - 1];
                g_state.devices[g_state.device_count - 1] = NULL;
            } else {
                g_state.devices[i] = NULL;
            }
            g_state.device_count--;
            free_device(to_free);
            break;
        }
    }
}

static void on_core_done(void *data, uint32_t id, int seq) {
    BackendState *st = (BackendState *)data;
    if (id == PW_ID_CORE && seq == st->sync_seq) {
        st->sync_done = true;
        pw_thread_loop_signal(st->loop, false);
    }
}

static void on_core_error(void *data, uint32_t id, int seq, int res, const char *message) {
    (void)message;
    BackendState *st = (BackendState *)data;
    if (id == PW_ID_CORE && seq == st->sync_seq) {
        st->sync_done = true;
        st->sync_error = res;
        pw_thread_loop_signal(st->loop, false);
    }
}

static const struct pw_core_events core_events = {
    PW_VERSION_CORE_EVENTS,
    .done = on_core_done,
    .error = on_core_error,
};

static void device_event_info(void *data, const struct pw_device_info *info) {
    DeviceItem *dev = (DeviceItem *)data;
    if (!dev || !info) return;

    if (info->change_mask & PW_DEVICE_CHANGE_MASK_PARAMS) {
        for (uint32_t i = 0; i < info->n_params; i++) {
            if (info->params[i].id == SPA_PARAM_Route) {
                pw_device_enum_params(dev->proxy, 0, SPA_PARAM_Route, 0, -1, NULL);
            }
        }
    }
}

static void device_event_param(void *data, int seq, uint32_t id, uint32_t index, uint32_t next, const struct spa_pod *param) {
    (void)seq; (void)index; (void)next;
    DeviceItem *dev = (DeviceItem *)data;
    if (!dev || !param || id != SPA_PARAM_Route) return;

    const struct spa_pod_object *obj = (const struct spa_pod_object *)param;
    const struct spa_pod_prop *prop;
    int32_t r_index = -1;
    int32_t r_device = -1;
    const struct spa_pod *props_pod = NULL;

    SPA_POD_OBJECT_FOREACH(obj, prop) {
        if (prop->key == SPA_PARAM_ROUTE_index) {
            spa_pod_get_int(&prop->value, &r_index);
        } else if (prop->key == SPA_PARAM_ROUTE_device) {
            spa_pod_get_int(&prop->value, &r_device);
        } else if (prop->key == SPA_PARAM_ROUTE_props) {
            props_pod = &prop->value;
        }
    }

    if (r_index < 0 || r_device < 0) return;

    for (uint32_t i = 0; i < g_state.node_count; i++) {
        NodeItem *node = g_state.nodes[i];
        if (node && node->device_id == dev->id && node->card_profile_device == r_device) {
            node->route_index = r_index;
            if (props_pod) {
                const struct spa_pod_prop *pp;
                SPA_POD_OBJECT_FOREACH((const struct spa_pod_object *)props_pod, pp) {
                    if (pp->key == SPA_PROP_volume) {
                        float v = 1.0f;
                        if (spa_pod_get_float(&pp->value, &v) >= 0) node->volume = v;
                    } else if (pp->key == SPA_PROP_mute) {
                        bool m = false;
                        if (spa_pod_get_bool(&pp->value, &m) >= 0) node->mute = m;
                    } else if (pp->key == SPA_PROP_channelVolumes) {
                        uint32_t n_vols = 0;
                        float *vols = (float *)spa_pod_get_array(&pp->value, &n_vols);
                        if (vols && n_vols > 0) {
                            node->n_channels = n_vols > MAX_CHANNELS ? MAX_CHANNELS : n_vols;
                            for (uint32_t c = 0; c < node->n_channels; c++) {
                                node->channel_volumes[c] = cbrtf(vols[c]);
                            }
                            if (node->n_channels > 0) node->volume = node->channel_volumes[0];
                        }
                    }
                }
            }
        }
    }
}

static const struct pw_device_events device_events = {
    PW_VERSION_DEVICE_EVENTS,
    .info = device_event_info,
    .param = device_event_param,
};

static void node_event_info(void *data, const struct pw_node_info *info) {
    NodeItem *node = (NodeItem *)data;
    if (!node || !info) return;

    if (info->change_mask & PW_NODE_CHANGE_MASK_PROPS) {
        const char *name = spa_dict_lookup(info->props, PW_KEY_NODE_NAME);
        const char *desc = spa_dict_lookup(info->props, PW_KEY_NODE_DESCRIPTION);
        const char *nick = spa_dict_lookup(info->props, PW_KEY_NODE_NICK);
        const char *mclass = spa_dict_lookup(info->props, PW_KEY_MEDIA_CLASS);
        const char *virt = spa_dict_lookup(info->props, PW_KEY_NODE_VIRTUAL);
        const char *corked = spa_dict_lookup(info->props, "pulse.corked");
        const char *client = spa_dict_lookup(info->props, PW_KEY_CLIENT_ID);
        const char *dev_str = spa_dict_lookup(info->props, PW_KEY_DEVICE_ID);
        const char *prof_str = spa_dict_lookup(info->props, "card.profile.device");

        if (name) {
            free(node->name);
            node->name = strdup(name);
        }
        if (desc || nick || name) {
            free(node->description);
            node->description = desc ? strdup(desc) : (nick ? strdup(nick) : strdup(name));
        }
        if (mclass) {
            free(node->media_class);
            node->media_class = strdup(mclass);
        }
        if (virt) node->is_virtual = (strcmp(virt, "true") == 0);
        if (corked) node->is_corked = (strcmp(corked, "true") == 0);
        if (client) node->client_id = (uint32_t)parse_int(client, 0);
        if (dev_str) node->device_id = (uint32_t)parse_int(dev_str, 0);
        if (prof_str) node->card_profile_device = (int32_t)parse_int(prof_str, -1);

        if (node->props) pw_properties_free(node->props);
        node->props = pw_properties_new_dict(info->props);
    }

    if (info->change_mask & PW_NODE_CHANGE_MASK_STATE) {
        free(node->state);
        switch (info->state) {
            case PW_NODE_STATE_RUNNING:
                node->state = strdup("RUNNING");
                break;
            case PW_NODE_STATE_IDLE:
                node->state = strdup("IDLE");
                break;
            case PW_NODE_STATE_SUSPENDED:
                node->state = strdup("SUSPENDED");
                break;
            case PW_NODE_STATE_CREATING:
                node->state = strdup("CREATING");
                break;
            case PW_NODE_STATE_ERROR:
                node->state = strdup("ERROR");
                break;
            default:
                node->state = strdup("UNKNOWN");
                break;
        }
    }

    if (info->change_mask & PW_NODE_CHANGE_MASK_PARAMS) {
        for (uint32_t i = 0; i < info->n_params; i++) {
            if (info->params[i].id == SPA_PARAM_Props) {
                pw_node_enum_params(node->proxy, 0, SPA_PARAM_Props, 0, 1, NULL);
            }
        }
    }

    notify_js_event("devices-changed");
    notify_js_event("streams-changed");
}

static void node_event_param(void *data, int seq, uint32_t id, uint32_t index, uint32_t next, const struct spa_pod *param) {
    (void)seq; (void)index; (void)next;
    NodeItem *node = (NodeItem *)data;
    if (!node || !param || id != SPA_PARAM_Props) return;

    const struct spa_pod_prop *prop;
    const struct spa_pod_object *obj = (const struct spa_pod_object *)param;

    SPA_POD_OBJECT_FOREACH(obj, prop) {
        if (prop->key == SPA_PROP_volume) {
            float v = 1.0f;
            if (spa_pod_get_float(&prop->value, &v) >= 0) {
                node->volume = v;
            }
        } else if (prop->key == SPA_PROP_mute) {
            bool m = false;
            if (spa_pod_get_bool(&prop->value, &m) >= 0) {
                node->mute = m;
            }
        } else if (prop->key == SPA_PROP_channelVolumes) {
            uint32_t n_vols = 0;
            float *vols = (float *)spa_pod_get_array(&prop->value, &n_vols);
            if (vols && n_vols > 0) {
                node->n_channels = n_vols > MAX_CHANNELS ? MAX_CHANNELS : n_vols;
                for (uint32_t i = 0; i < node->n_channels; i++) {
                    node->channel_volumes[i] = cbrtf(vols[i]);
                }
                if (node->n_channels > 0) node->volume = node->channel_volumes[0];
            }
        }
    }

    notify_js_event("devices-changed");
    notify_js_event("streams-changed");
}

static const struct pw_node_events node_events = {
    PW_VERSION_NODE_EVENTS,
    .info = node_event_info,
    .param = node_event_param,
};

static void link_event_info(void *data, const struct pw_link_info *info) {
    LinkItem *link = (LinkItem *)data;
    if (!link || !info) return;
    link->output_node_id = info->output_node_id;
    link->input_node_id = info->input_node_id;
    notify_js_event("streams-changed");
}

static const struct pw_link_events link_events = {
    PW_VERSION_LINK_EVENTS,
    .info = link_event_info,
};

static int metadata_event_property(void *data, uint32_t subject, const char *key, const char *type, const char *value) {
    (void)data; (void)type;
    if (subject == 0 && key) {
        if (strcmp(key, "default.audio.sink") == 0 || strcmp(key, "default.configured.audio.sink") == 0) {
            if (value) {
                const char *p = strstr(value, "\"name\":\"");
                if (p) {
                    p += 8;
                    const char *end = strchr(p, '\"');
                    if (end) {
                        size_t len = (size_t)(end - p);
                        if (len < sizeof(g_state.metadata.default_sink)) {
                            memcpy(g_state.metadata.default_sink, p, len);
                            g_state.metadata.default_sink[len] = '\0';
                        }
                    }
                } else {
                    snprintf(g_state.metadata.default_sink, sizeof(g_state.metadata.default_sink), "%s", value);
                }
            } else if (strcmp(key, "default.audio.sink") == 0) {
                g_state.metadata.default_sink[0] = '\0';
            }
            notify_js_event("default-sink-changed");
        }
    }
    return 0;
}

static const struct pw_metadata_events metadata_events = {
    PW_VERSION_METADATA_EVENTS,
    .property = metadata_event_property,
};

static void registry_event_global(void *data, uint32_t id, uint32_t permissions, const char *type, uint32_t version, const struct spa_dict *props) {
    (void)data; (void)permissions; (void)version;
    if (strcmp(type, PW_TYPE_INTERFACE_Node) == 0) {
        if (g_state.node_count >= MAX_NODES) return;
        NodeItem *node = (NodeItem *)calloc(1, sizeof(NodeItem));
        if (!node) return;

        node->id = id;
        node->volume = 1.0f;
        for (int i = 0; i < MAX_CHANNELS; i++) node->channel_volumes[i] = 1.0f;
        node->n_channels = 2;
        node->mute = false;
        node->route_index = -1;
        node->card_profile_device = -1;

        if (props) {
            const char *name = spa_dict_lookup(props, PW_KEY_NODE_NAME);
            const char *desc = spa_dict_lookup(props, PW_KEY_NODE_DESCRIPTION);
            const char *nick = spa_dict_lookup(props, PW_KEY_NODE_NICK);
            const char *mclass = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
            const char *virt = spa_dict_lookup(props, PW_KEY_NODE_VIRTUAL);
            const char *corked = spa_dict_lookup(props, "pulse.corked");
            const char *client = spa_dict_lookup(props, PW_KEY_CLIENT_ID);
            const char *dev_str = spa_dict_lookup(props, PW_KEY_DEVICE_ID);
            const char *prof_str = spa_dict_lookup(props, "card.profile.device");

            if (name) node->name = strdup(name);
            if (desc) node->description = strdup(desc);
            else if (nick) node->description = strdup(nick);
            else if (name) node->description = strdup(name);

            if (mclass) node->media_class = strdup(mclass);
            node->is_virtual = (virt && strcmp(virt, "true") == 0);
            node->is_corked = (corked && strcmp(corked, "true") == 0);
            node->client_id = client ? (uint32_t)parse_int(client, 0) : 0;
            node->device_id = dev_str ? (uint32_t)parse_int(dev_str, 0) : 0;
            node->card_profile_device = prof_str ? (int32_t)parse_int(prof_str, -1) : -1;
            node->props = pw_properties_new_dict(props);
        }

        node->proxy = (struct pw_node *)pw_registry_bind(g_state.registry, id, type, PW_VERSION_NODE, 0);
        if (node->proxy) {
            pw_node_add_listener(node->proxy, &node->listener, &node_events, node);
            pw_node_subscribe_params(node->proxy, (uint32_t[]){ SPA_PARAM_Props }, 1);
            g_state.nodes[g_state.node_count++] = node;
        } else {
            free(node->name);
            free(node->description);
            free(node->media_class);
            if (node->props) pw_properties_free(node->props);
            free(node);
        }
    } else if (strcmp(type, PW_TYPE_INTERFACE_Link) == 0) {
        if (g_state.link_count >= MAX_LINKS) return;
        LinkItem *link = (LinkItem *)calloc(1, sizeof(LinkItem));
        if (!link) return;

        link->id = id;
        if (props) {
            const char *out_node = spa_dict_lookup(props, PW_KEY_LINK_OUTPUT_NODE);
            const char *in_node = spa_dict_lookup(props, PW_KEY_LINK_INPUT_NODE);
            if (out_node) link->output_node_id = (uint32_t)parse_int(out_node, 0);
            if (in_node) link->input_node_id = (uint32_t)parse_int(in_node, 0);
        }

        link->proxy = (struct pw_link *)pw_registry_bind(g_state.registry, id, type, PW_VERSION_LINK, 0);
        if (link->proxy) {
            pw_link_add_listener(link->proxy, &link->listener, &link_events, link);
            g_state.links[g_state.link_count++] = link;
        } else {
            free(link);
        }
    } else if (strcmp(type, PW_TYPE_INTERFACE_Device) == 0) {
        if (g_state.device_count >= MAX_DEVICES) return;
        DeviceItem *dev = (DeviceItem *)calloc(1, sizeof(DeviceItem));
        if (!dev) return;

        dev->id = id;
        dev->proxy = (struct pw_device *)pw_registry_bind(g_state.registry, id, type, PW_VERSION_DEVICE, 0);
        if (dev->proxy) {
            pw_device_add_listener(dev->proxy, &dev->listener, &device_events, dev);
            pw_device_subscribe_params(dev->proxy, (uint32_t[]){ SPA_PARAM_Route }, 1);
            g_state.devices[g_state.device_count++] = dev;
        } else {
            free(dev);
        }
    } else if (strcmp(type, PW_TYPE_INTERFACE_Metadata) == 0) {
        const char *name = props ? spa_dict_lookup(props, PW_KEY_METADATA_NAME) : NULL;
        if (name && strcmp(name, "default") == 0) {
            g_state.metadata.id = id;
            g_state.metadata.proxy = (struct pw_metadata *)pw_registry_bind(g_state.registry, id, type, PW_VERSION_METADATA, 0);
            if (g_state.metadata.proxy) {
                pw_metadata_add_listener(g_state.metadata.proxy, &g_state.metadata.listener, &metadata_events, &g_state.metadata);
                g_state.has_metadata = true;
            }
        }
    }
}

static void registry_event_global_remove(void *data, uint32_t id) {
    (void)data;
    remove_node(id);
    remove_link(id);
    remove_device(id);
    if (g_state.has_metadata && g_state.metadata.id == id) {
        if (g_state.metadata.proxy) {
            spa_hook_remove(&g_state.metadata.listener);
            pw_proxy_destroy((struct pw_proxy *)g_state.metadata.proxy);
            g_state.metadata.proxy = NULL;
        }
        g_state.has_metadata = false;
    }
}

static const struct pw_registry_events registry_events = {
    PW_VERSION_REGISTRY_EVENTS,
    .global = registry_event_global,
    .global_remove = registry_event_global_remove,
};

static void js_event_call_js(napi_env env, napi_value js_cb, void *context, void *data) {
    (void)context;
    if (env == NULL || js_cb == NULL) return;
    const char *event_name = (const char *)data;
    napi_value argv[1];
    napi_create_string_utf8(env, event_name, NAPI_AUTO_LENGTH, &argv[0]);
    napi_value undefined;
    napi_get_undefined(env, &undefined);
    napi_call_function(env, undefined, js_cb, 1, argv, NULL);
}

static int do_roundtrip_locked(int timeout_sec) {
    g_state.sync_done = false;
    g_state.sync_error = 0;
    g_state.sync_seq = pw_core_sync(g_state.core, PW_ID_CORE, ++g_state.seq_counter);

    while (!g_state.sync_done) {
        if (pw_thread_loop_timed_wait(g_state.loop, timeout_sec) != 0) {
            return -ETIMEDOUT;
        }
    }
    return g_state.sync_error;
}

static napi_value Method_Init(napi_env env, napi_callback_info info) {
    (void)info;
    if (g_state.initialized) {
        napi_value val;
        napi_get_boolean(env, true, &val);
        return val;
    }

    pw_init(NULL, NULL);

    g_state.next_module_id = 1;
    g_state.loop = pw_thread_loop_new("speakerflow-pipewire", NULL);
    if (!g_state.loop) {
        napi_throw_error(env, NULL, "Failed to create PipeWire thread loop");
        return NULL;
    }

    g_state.context = pw_context_new(pw_thread_loop_get_loop(g_state.loop), NULL, 0);
    if (!g_state.context) {
        pw_thread_loop_destroy(g_state.loop);
        g_state.loop = NULL;
        napi_throw_error(env, NULL, "Failed to create PipeWire context");
        return NULL;
    }

    if (pw_thread_loop_start(g_state.loop) < 0) {
        pw_context_destroy(g_state.context);
        pw_thread_loop_destroy(g_state.loop);
        g_state.loop = NULL;
        g_state.context = NULL;
        napi_throw_error(env, NULL, "Failed to start PipeWire thread loop");
        return NULL;
    }

    pw_thread_loop_lock(g_state.loop);

    g_state.core = pw_context_connect(g_state.context, NULL, 0);
    if (!g_state.core) {
        pw_thread_loop_unlock(g_state.loop);
        pw_thread_loop_stop(g_state.loop);
        pw_context_destroy(g_state.context);
        pw_thread_loop_destroy(g_state.loop);
        g_state.loop = NULL;
        g_state.context = NULL;
        napi_throw_error(env, NULL, "Failed to connect to PipeWire core socket");
        return NULL;
    }

    pw_core_add_listener(g_state.core, &g_state.core_listener, &core_events, &g_state);

    g_state.registry = pw_core_get_registry(g_state.core, PW_VERSION_REGISTRY, 0);
    if (!g_state.registry) {
        spa_hook_remove(&g_state.core_listener);
        pw_core_disconnect(g_state.core);
        pw_thread_loop_unlock(g_state.loop);
        pw_thread_loop_stop(g_state.loop);
        pw_context_destroy(g_state.context);
        pw_thread_loop_destroy(g_state.loop);
        g_state.loop = NULL;
        g_state.context = NULL;
        g_state.core = NULL;
        napi_throw_error(env, NULL, "Failed to get PipeWire registry");
        return NULL;
    }

    pw_registry_add_listener(g_state.registry, &g_state.registry_listener, &registry_events, &g_state);

    // Two roundtrip syncs to ensure complete initialization:
    // Roundtrip 1: discovers and binds all global objects (nodes, links, devices, metadata)
    do_roundtrip_locked(2);
    // Roundtrip 2: retrieves initial info, params (volume/mute), and metadata properties
    do_roundtrip_locked(2);

    pw_thread_loop_unlock(g_state.loop);

    g_state.initialized = true;
    napi_value val;
    napi_get_boolean(env, true, &val);
    return val;
}

static napi_value Method_SetEventCallback(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1];
    napi_get_cb_info(env, info, &argc, argv, NULL, NULL);

    if (argc < 1) {
        napi_throw_type_error(env, NULL, "Callback required");
        return NULL;
    }

    if (g_state.tsfn) {
        napi_release_threadsafe_function(g_state.tsfn, napi_tsfn_release);
        g_state.tsfn = NULL;
    }

    napi_value resource_name;
    napi_create_string_utf8(env, "SpeakerFlowPipeWireEvents", NAPI_AUTO_LENGTH, &resource_name);
    napi_create_threadsafe_function(env, argv[0], NULL, resource_name, 0, 1, NULL, NULL, NULL, js_event_call_js, &g_state.tsfn);

    napi_value undefined;
    napi_get_undefined(env, &undefined);
    return undefined;
}

static napi_value Method_ListSinks(napi_env env, napi_callback_info info) {
    (void)info;
    if (!g_state.initialized || !g_state.loop) {
        napi_value empty_arr;
        napi_create_array(env, &empty_arr);
        return empty_arr;
    }

    pw_thread_loop_lock(g_state.loop);

    napi_value arr;
    napi_create_array(env, &arr);
    uint32_t out_idx = 0;

    for (uint32_t i = 0; i < g_state.node_count; i++) {
        NodeItem *node = g_state.nodes[i];
        if (!node || !node->media_class || strcmp(node->media_class, "Audio/Sink") != 0) continue;

        napi_value obj;
        napi_create_object(env, &obj);

        napi_value id_val, name_val, desc_val, state_val, virt_val, mute_val;
        napi_create_uint32(env, node->id, &id_val);
        napi_create_string_utf8(env, node->name ? node->name : "", NAPI_AUTO_LENGTH, &name_val);
        napi_create_string_utf8(env, node->description ? node->description : (node->name ? node->name : ""), NAPI_AUTO_LENGTH, &desc_val);
        napi_create_string_utf8(env, node->state ? node->state : "IDLE", NAPI_AUTO_LENGTH, &state_val);
        napi_get_boolean(env, node->is_virtual, &virt_val);
        napi_get_boolean(env, node->mute, &mute_val);

        napi_set_named_property(env, obj, "index", id_val);
        napi_set_named_property(env, obj, "name", name_val);
        napi_set_named_property(env, obj, "description", desc_val);
        napi_set_named_property(env, obj, "state", state_val);
        napi_set_named_property(env, obj, "isVirtual", virt_val);
        napi_set_named_property(env, obj, "mute", mute_val);

        // Volume object
        napi_value vol_obj;
        napi_create_object(env, &vol_obj);
        char pct_buf[32];
        float ch1 = (node->n_channels > 0) ? node->channel_volumes[0] : node->volume;
        float ch2 = (node->n_channels > 1) ? node->channel_volumes[1] : ch1;

        snprintf(pct_buf, sizeof(pct_buf), "%d%%", (int)(ch1 * 100.0f + 0.5f));
        napi_value fl_obj, fl_pct;
        napi_create_object(env, &fl_obj);
        napi_create_string_utf8(env, pct_buf, NAPI_AUTO_LENGTH, &fl_pct);
        napi_set_named_property(env, fl_obj, "value_percent", fl_pct);
        napi_set_named_property(env, vol_obj, "front-left", fl_obj);

        snprintf(pct_buf, sizeof(pct_buf), "%d%%", (int)(ch2 * 100.0f + 0.5f));
        napi_value fr_obj, fr_pct;
        napi_create_object(env, &fr_obj);
        napi_create_string_utf8(env, pct_buf, NAPI_AUTO_LENGTH, &fr_pct);
        napi_set_named_property(env, fr_obj, "value_percent", fr_pct);
        napi_set_named_property(env, vol_obj, "front-right", fr_obj);

        napi_set_named_property(env, obj, "volume", vol_obj);

        // Properties dictionary
        napi_value props_obj;
        napi_create_object(env, &props_obj);
        if (node->props) {
            const struct spa_dict_item *item;
            spa_dict_for_each(item, &node->props->dict) {
                napi_value k_val;
                napi_create_string_utf8(env, item->value, NAPI_AUTO_LENGTH, &k_val);
                napi_set_named_property(env, props_obj, item->key, k_val);
            }
        }
        napi_set_named_property(env, obj, "properties", props_obj);

        napi_set_element(env, arr, out_idx++, obj);
    }

    pw_thread_loop_unlock(g_state.loop);
    return arr;
}

static napi_value Method_ListSinkInputs(napi_env env, napi_callback_info info) {
    (void)info;
    if (!g_state.initialized || !g_state.loop) {
        napi_value empty_arr;
        napi_create_array(env, &empty_arr);
        return empty_arr;
    }

    pw_thread_loop_lock(g_state.loop);

    napi_value arr;
    napi_create_array(env, &arr);
    uint32_t out_idx = 0;

    for (uint32_t i = 0; i < g_state.node_count; i++) {
        NodeItem *node = g_state.nodes[i];
        if (!node || !node->media_class || strcmp(node->media_class, "Stream/Output/Audio") != 0) continue;

        uint32_t target_sink_id = 0;
        for (uint32_t l = 0; l < g_state.link_count; l++) {
            if (g_state.links[l] && g_state.links[l]->output_node_id == node->id) {
                target_sink_id = g_state.links[l]->input_node_id;
                break;
            }
        }

        napi_value obj;
        napi_create_object(env, &obj);

        napi_value id_val, sink_val, client_val, mute_val, corked_val;
        napi_create_uint32(env, node->id, &id_val);
        if (target_sink_id > 0) {
            napi_create_uint32(env, target_sink_id, &sink_val);
        } else {
            napi_get_null(env, &sink_val);
        }
        napi_create_uint32(env, node->client_id, &client_val);
        napi_get_boolean(env, node->mute, &mute_val);
        napi_get_boolean(env, node->is_corked || (node->state && strcmp(node->state, "SUSPENDED") == 0), &corked_val);

        napi_set_named_property(env, obj, "index", id_val);
        napi_set_named_property(env, obj, "sink", sink_val);
        napi_set_named_property(env, obj, "client", client_val);
        napi_set_named_property(env, obj, "mute", mute_val);
        napi_set_named_property(env, obj, "corked", corked_val);

        // Volume object
        napi_value vol_obj;
        napi_create_object(env, &vol_obj);
        char pct_buf[32];
        float ch1 = (node->n_channels > 0) ? node->channel_volumes[0] : node->volume;
        float ch2 = (node->n_channels > 1) ? node->channel_volumes[1] : ch1;

        snprintf(pct_buf, sizeof(pct_buf), "%d%%", (int)(ch1 * 100.0f + 0.5f));
        napi_value fl_obj, fl_pct;
        napi_create_object(env, &fl_obj);
        napi_create_string_utf8(env, pct_buf, NAPI_AUTO_LENGTH, &fl_pct);
        napi_set_named_property(env, fl_obj, "value_percent", fl_pct);
        napi_set_named_property(env, vol_obj, "front-left", fl_obj);

        snprintf(pct_buf, sizeof(pct_buf), "%d%%", (int)(ch2 * 100.0f + 0.5f));
        napi_value fr_obj, fr_pct;
        napi_create_object(env, &fr_obj);
        napi_create_string_utf8(env, pct_buf, NAPI_AUTO_LENGTH, &fr_pct);
        napi_set_named_property(env, fr_obj, "value_percent", fr_pct);
        napi_set_named_property(env, vol_obj, "front-right", fr_obj);

        napi_set_named_property(env, obj, "volume", vol_obj);

        // Properties dictionary
        napi_value props_obj;
        napi_create_object(env, &props_obj);
        if (node->props) {
            const struct spa_dict_item *item;
            spa_dict_for_each(item, &node->props->dict) {
                napi_value k_val;
                napi_create_string_utf8(env, item->value, NAPI_AUTO_LENGTH, &k_val);
                napi_set_named_property(env, props_obj, item->key, k_val);
            }
        }
        napi_set_named_property(env, obj, "properties", props_obj);

        napi_set_element(env, arr, out_idx++, obj);
    }

    pw_thread_loop_unlock(g_state.loop);
    return arr;
}

static napi_value Method_GetDefaultSink(napi_env env, napi_callback_info info) {
    (void)info;
    if (!g_state.initialized || !g_state.loop) {
        napi_value null_val;
        napi_get_null(env, &null_val);
        return null_val;
    }

    pw_thread_loop_lock(g_state.loop);
    napi_value res;
    if (g_state.has_metadata && g_state.metadata.default_sink[0] != '\0') {
        napi_create_string_utf8(env, g_state.metadata.default_sink, NAPI_AUTO_LENGTH, &res);
    } else {
        napi_get_null(env, &res);
    }
    pw_thread_loop_unlock(g_state.loop);

    return res;
}

static napi_value Method_SetDefaultSink(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1];
    napi_get_cb_info(env, info, &argc, argv, NULL, NULL);

    if (argc < 1) {
        napi_throw_type_error(env, NULL, "Sink name required");
        return NULL;
    }

    char sink_name[256];
    size_t len;
    napi_get_value_string_utf8(env, argv[0], sink_name, sizeof(sink_name), &len);

    if (!g_state.initialized || !g_state.has_metadata || !g_state.metadata.proxy) {
        napi_throw_error(env, NULL, "PipeWire metadata proxy unavailable");
        return NULL;
    }

    pw_thread_loop_lock(g_state.loop);
    char json_buf[512];
    snprintf(json_buf, sizeof(json_buf), "{\"name\":\"%s\"}", sink_name);
    pw_metadata_set_property(g_state.metadata.proxy, 0, "default.configured.audio.sink", "Spa:String:JSON", json_buf);
    pw_metadata_set_property(g_state.metadata.proxy, 0, "default.audio.sink", "Spa:String:JSON", json_buf);
    snprintf(g_state.metadata.default_sink, sizeof(g_state.metadata.default_sink), "%s", sink_name);
    pw_thread_loop_unlock(g_state.loop);

    notify_js_event("default-sink-changed");

    napi_value true_val;
    napi_get_boolean(env, true, &true_val);
    return true_val;
}

static napi_value Method_MoveSinkInput(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2];
    napi_get_cb_info(env, info, &argc, argv, NULL, NULL);

    if (argc < 2) {
        napi_throw_type_error(env, NULL, "streamId and targetSinkName required");
        return NULL;
    }

    uint32_t stream_id;
    napi_get_value_uint32(env, argv[0], &stream_id);

    char target_name[256];
    size_t len;
    napi_get_value_string_utf8(env, argv[1], target_name, sizeof(target_name), &len);

    if (!g_state.initialized || !g_state.has_metadata || !g_state.metadata.proxy) {
        napi_throw_error(env, NULL, "PipeWire metadata proxy unavailable");
        return NULL;
    }

    pw_thread_loop_lock(g_state.loop);
    pw_metadata_set_property(g_state.metadata.proxy, stream_id, "target.node", NULL, target_name);
    pw_metadata_set_property(g_state.metadata.proxy, stream_id, "target.object", NULL, target_name);
    pw_thread_loop_unlock(g_state.loop);

    notify_js_event("streams-changed");

    napi_value true_val;
    napi_get_boolean(env, true, &true_val);
    return true_val;
}

static napi_value Method_SetNodeVolume(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2];
    napi_get_cb_info(env, info, &argc, argv, NULL, NULL);

    if (argc < 2) {
        napi_throw_type_error(env, NULL, "nodeId and volumePercent required");
        return NULL;
    }

    uint32_t node_id;
    napi_get_value_uint32(env, argv[0], &node_id);

    double vol_percent;
    napi_get_value_double(env, argv[1], &vol_percent);

    float vol_cubic = (float)(vol_percent / 100.0);
    if (vol_cubic < 0.0f) vol_cubic = 0.0f;
    if (vol_cubic > 1.5f) vol_cubic = 1.5f;

    if (!g_state.initialized || !g_state.loop) {
        napi_throw_error(env, NULL, "PipeWire not initialized");
        return NULL;
    }

    pw_thread_loop_lock(g_state.loop);

    NodeItem *node = find_node(node_id);
    if (node) {
        float vol_linear = vol_cubic * vol_cubic * vol_cubic;
        uint32_t n_ch = node->n_channels > 0 ? (node->n_channels > MAX_CHANNELS ? MAX_CHANNELS : node->n_channels) : 2;
        float channel_vols[MAX_CHANNELS];
        for (uint32_t i = 0; i < n_ch; i++) channel_vols[i] = vol_linear;

        // If this node is attached to an ALSA hardware device route, update device route
        if (node->device_id > 0 && node->route_index >= 0) {
            DeviceItem *dev = find_device(node->device_id);
            if (dev && dev->proxy) {
                uint8_t buffer[1024];
                struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
                struct spa_pod_frame f[2];

                spa_pod_builder_push_object(&b, &f[0], SPA_TYPE_OBJECT_ParamRoute, SPA_PARAM_Route);
                spa_pod_builder_add(&b,
                    SPA_PARAM_ROUTE_index, SPA_POD_Int(node->route_index),
                    SPA_PARAM_ROUTE_device, SPA_POD_Int(node->card_profile_device),
                    0);
                spa_pod_builder_prop(&b, SPA_PARAM_ROUTE_props, 0);
                spa_pod_builder_push_object(&b, &f[1], SPA_TYPE_OBJECT_Props, SPA_PARAM_Props);
                spa_pod_builder_add(&b,
                    SPA_PROP_channelVolumes, SPA_POD_Array(sizeof(float), SPA_TYPE_Float, n_ch, channel_vols),
                    0);
                spa_pod_builder_pop(&b, &f[1]);
                spa_pod_builder_add(&b,
                    SPA_PARAM_ROUTE_save, SPA_POD_Bool(true),
                    0);
                struct spa_pod *param = spa_pod_builder_pop(&b, &f[0]);
                pw_device_set_param(dev->proxy, SPA_PARAM_Route, 0, param);
            }
        }

        // Also update node Props (applies directly to virtual sinks, streams, and software mixers)
        if (node->proxy) {
            uint8_t buffer[1024];
            struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
            struct spa_pod_frame f;

            spa_pod_builder_push_object(&b, &f, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props);
            spa_pod_builder_add(&b,
                SPA_PROP_volume, SPA_POD_Float(vol_cubic),
                SPA_PROP_channelVolumes, SPA_POD_Array(sizeof(float), SPA_TYPE_Float, n_ch, channel_vols),
                0);
            struct spa_pod *param = spa_pod_builder_pop(&b, &f);
            pw_node_set_param(node->proxy, SPA_PARAM_Props, 0, param);
        }

        node->volume = vol_cubic;
        for (uint32_t i = 0; i < n_ch; i++) node->channel_volumes[i] = vol_cubic;
    }

    pw_thread_loop_unlock(g_state.loop);

    notify_js_event("devices-changed");
    notify_js_event("streams-changed");

    napi_value true_val;
    napi_get_boolean(env, true, &true_val);
    return true_val;
}

static napi_value Method_SetNodeMute(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2];
    napi_get_cb_info(env, info, &argc, argv, NULL, NULL);

    if (argc < 2) {
        napi_throw_type_error(env, NULL, "nodeId and muted boolean required");
        return NULL;
    }

    uint32_t node_id;
    napi_get_value_uint32(env, argv[0], &node_id);

    bool muted;
    napi_get_value_bool(env, argv[1], &muted);

    if (!g_state.initialized || !g_state.loop) {
        napi_throw_error(env, NULL, "PipeWire not initialized");
        return NULL;
    }

    pw_thread_loop_lock(g_state.loop);

    NodeItem *node = find_node(node_id);
    if (node) {
        // If this node is attached to an ALSA hardware device route, update device route mute
        if (node->device_id > 0 && node->route_index >= 0) {
            DeviceItem *dev = find_device(node->device_id);
            if (dev && dev->proxy) {
                uint8_t buffer[1024];
                struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
                struct spa_pod_frame f[2];

                spa_pod_builder_push_object(&b, &f[0], SPA_TYPE_OBJECT_ParamRoute, SPA_PARAM_Route);
                spa_pod_builder_add(&b,
                    SPA_PARAM_ROUTE_index, SPA_POD_Int(node->route_index),
                    SPA_PARAM_ROUTE_device, SPA_POD_Int(node->card_profile_device),
                    0);
                spa_pod_builder_prop(&b, SPA_PARAM_ROUTE_props, 0);
                spa_pod_builder_push_object(&b, &f[1], SPA_TYPE_OBJECT_Props, SPA_PARAM_Props);
                spa_pod_builder_add(&b,
                    SPA_PROP_mute, SPA_POD_Bool(muted),
                    0);
                spa_pod_builder_pop(&b, &f[1]);
                spa_pod_builder_add(&b,
                    SPA_PARAM_ROUTE_save, SPA_POD_Bool(true),
                    0);
                struct spa_pod *param = spa_pod_builder_pop(&b, &f[0]);
                pw_device_set_param(dev->proxy, SPA_PARAM_Route, 0, param);
            }
        }

        // Also update node Props
        if (node->proxy) {
            uint8_t buffer[1024];
            struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
            struct spa_pod_frame f;

            spa_pod_builder_push_object(&b, &f, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props);
            spa_pod_builder_add(&b,
                SPA_PROP_mute, SPA_POD_Bool(muted),
                0);
            struct spa_pod *param = spa_pod_builder_pop(&b, &f);
            pw_node_set_param(node->proxy, SPA_PARAM_Props, 0, param);
        }

        node->mute = muted;
    }

    pw_thread_loop_unlock(g_state.loop);

    notify_js_event("devices-changed");
    notify_js_event("streams-changed");

    napi_value true_val;
    napi_get_boolean(env, true, &true_val);
    return true_val;
}

static napi_value Method_LoadModule(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2];
    napi_get_cb_info(env, info, &argc, argv, NULL, NULL);

    if (argc < 1) {
        napi_throw_type_error(env, NULL, "Module name required");
        return NULL;
    }

    char mod_name[256];
    size_t len;
    napi_get_value_string_utf8(env, argv[0], mod_name, sizeof(mod_name), &len);

    char mod_args[2048] = {0};
    if (argc > 1) {
        napi_valuetype t;
        napi_typeof(env, argv[1], &t);
        if (t == napi_string) {
            napi_get_value_string_utf8(env, argv[1], mod_args, sizeof(mod_args), &len);
        }
    }

    if (!g_state.initialized || !g_state.loop || !g_state.context) {
        napi_throw_error(env, NULL, "PipeWire context not initialized");
        return NULL;
    }

    pw_thread_loop_lock(g_state.loop);

    if (g_state.module_count >= MAX_LOADED_MODULES) {
        pw_thread_loop_unlock(g_state.loop);
        napi_throw_error(env, NULL, "Max loaded modules reached");
        return NULL;
    }

    struct pw_impl_module *mod = pw_context_load_module(
        g_state.context,
        mod_name,
        mod_args[0] != '\0' ? mod_args : NULL,
        NULL
    );

    if (!mod) {
        pw_thread_loop_unlock(g_state.loop);
        napi_throw_error(env, NULL, "Failed to load PipeWire module");
        return NULL;
    }

    uint32_t mod_id = g_state.next_module_id++;
    LoadedModuleItem *item = &g_state.modules[g_state.module_count++];
    item->id = mod_id;
    item->mod = mod;

    pw_thread_loop_unlock(g_state.loop);

    napi_value res;
    napi_create_uint32(env, mod_id, &res);
    return res;
}

static napi_value Method_UnloadModule(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1];
    napi_get_cb_info(env, info, &argc, argv, NULL, NULL);

    if (argc < 1) {
        napi_throw_type_error(env, NULL, "moduleId required");
        return NULL;
    }

    uint32_t mod_id;
    napi_get_value_uint32(env, argv[0], &mod_id);

    if (!g_state.initialized || !g_state.loop) {
        napi_value f;
        napi_get_boolean(env, false, &f);
        return f;
    }

    pw_thread_loop_lock(g_state.loop);

    bool found = false;
    for (uint32_t i = 0; i < g_state.module_count; i++) {
        if (g_state.modules[i].id == mod_id) {
            if (g_state.modules[i].mod) {
                pw_impl_module_destroy(g_state.modules[i].mod);
                g_state.modules[i].mod = NULL;
            }
            if (i < g_state.module_count - 1) {
                g_state.modules[i] = g_state.modules[g_state.module_count - 1];
            }
            g_state.module_count--;
            found = true;
            break;
        }
    }

    pw_thread_loop_unlock(g_state.loop);

    napi_value res;
    napi_get_boolean(env, found, &res);
    return res;
}

static napi_value Method_Destroy(napi_env env, napi_callback_info info) {
    (void)info;
    if (!g_state.initialized) {
        napi_value val;
        napi_get_boolean(env, true, &val);
        return val;
    }

    g_state.initialized = false;

    if (g_state.loop) {
        pw_thread_loop_lock(g_state.loop);

        for (uint32_t i = 0; i < g_state.module_count; i++) {
            if (g_state.modules[i].mod) {
                pw_impl_module_destroy(g_state.modules[i].mod);
                g_state.modules[i].mod = NULL;
            }
        }
        g_state.module_count = 0;

        for (uint32_t i = 0; i < g_state.node_count; i++) {
            if (g_state.nodes[i]) {
                free_node(g_state.nodes[i]);
                g_state.nodes[i] = NULL;
            }
        }
        g_state.node_count = 0;

        for (uint32_t i = 0; i < g_state.link_count; i++) {
            if (g_state.links[i]) {
                free_link(g_state.links[i]);
                g_state.links[i] = NULL;
            }
        }
        g_state.link_count = 0;

        for (uint32_t i = 0; i < g_state.device_count; i++) {
            if (g_state.devices[i]) {
                free_device(g_state.devices[i]);
                g_state.devices[i] = NULL;
            }
        }
        g_state.device_count = 0;

        if (g_state.has_metadata && g_state.metadata.proxy) {
            spa_hook_remove(&g_state.metadata.listener);
            pw_proxy_destroy((struct pw_proxy *)g_state.metadata.proxy);
            g_state.metadata.proxy = NULL;
            g_state.has_metadata = false;
        }

        if (g_state.registry) {
            spa_hook_remove(&g_state.registry_listener);
            pw_proxy_destroy((struct pw_proxy *)g_state.registry);
            g_state.registry = NULL;
        }

        if (g_state.core) {
            spa_hook_remove(&g_state.core_listener);
            pw_core_disconnect(g_state.core);
            g_state.core = NULL;
        }

        pw_thread_loop_unlock(g_state.loop);

        pw_thread_loop_stop(g_state.loop);

        if (g_state.context) {
            pw_context_destroy(g_state.context);
            g_state.context = NULL;
        }

        pw_thread_loop_destroy(g_state.loop);
        g_state.loop = NULL;
    }

    if (g_state.tsfn) {
        napi_release_threadsafe_function(g_state.tsfn, napi_tsfn_release);
        g_state.tsfn = NULL;
    }

    napi_value val;
    napi_get_boolean(env, true, &val);
    return val;
}

NAPI_MODULE_INIT() {
    napi_value fn;

    napi_create_function(env, NULL, 0, Method_Init, NULL, &fn);
    napi_set_named_property(env, exports, "init", fn);

    napi_create_function(env, NULL, 0, Method_SetEventCallback, NULL, &fn);
    napi_set_named_property(env, exports, "setEventCallback", fn);

    napi_create_function(env, NULL, 0, Method_ListSinks, NULL, &fn);
    napi_set_named_property(env, exports, "listSinks", fn);

    napi_create_function(env, NULL, 0, Method_ListSinkInputs, NULL, &fn);
    napi_set_named_property(env, exports, "listSinkInputs", fn);

    napi_create_function(env, NULL, 0, Method_GetDefaultSink, NULL, &fn);
    napi_set_named_property(env, exports, "getDefaultSink", fn);

    napi_create_function(env, NULL, 0, Method_SetDefaultSink, NULL, &fn);
    napi_set_named_property(env, exports, "setDefaultSink", fn);

    napi_create_function(env, NULL, 0, Method_MoveSinkInput, NULL, &fn);
    napi_set_named_property(env, exports, "moveSinkInput", fn);

    napi_create_function(env, NULL, 0, Method_SetNodeVolume, NULL, &fn);
    napi_set_named_property(env, exports, "setNodeVolume", fn);

    napi_create_function(env, NULL, 0, Method_SetNodeMute, NULL, &fn);
    napi_set_named_property(env, exports, "setNodeMute", fn);

    napi_create_function(env, NULL, 0, Method_LoadModule, NULL, &fn);
    napi_set_named_property(env, exports, "loadModule", fn);

    napi_create_function(env, NULL, 0, Method_UnloadModule, NULL, &fn);
    napi_set_named_property(env, exports, "unloadModule", fn);

    napi_create_function(env, NULL, 0, Method_Destroy, NULL, &fn);
    napi_set_named_property(env, exports, "destroy", fn);

    return exports;
}
