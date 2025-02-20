/* GSK - The GTK Scene Kit
 *
 * Copyright 2016  Endless
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 */

/**
 * GskRenderer:
 *
 * `GskRenderer` is a class that renders a scene graph defined via a
 * tree of [class@Gsk.RenderNode] instances.
 *
 * Typically you will use a `GskRenderer` instance to repeatedly call
 * [method@Gsk.Renderer.render] to update the contents of its associated
 * [class@Gdk.Surface].
 *
 * It is necessary to realize a `GskRenderer` instance using
 * [method@Gsk.Renderer.realize] before calling [method@Gsk.Renderer.render],
 * in order to create the appropriate windowing system resources needed
 * to render the scene.
 */

#include "config.h"

#include "gskrendererprivate.h"

#include "gskcairorenderer.h"
#include "gskdebugprivate.h"
#include "gl/gskglrenderer.h"
#include "gskprofilerprivate.h"
#include "gskrendernodeprivate.h"

#include "gskenumtypes.h"

#include <graphene-gobject.h>
#include <cairo-gobject.h>
#include <gdk/gdk.h>

#ifdef GDK_WINDOWING_X11
#include <gdk/x11/gdkx.h>
#endif
#ifdef GDK_WINDOWING_WAYLAND
#include <gdk/wayland/gdkwayland.h>
#endif
#ifdef GDK_WINDOWING_BROADWAY
#include "broadway/gskbroadwayrenderer.h"
#endif
#ifdef GDK_RENDERING_VULKAN
#include "vulkan/gskvulkanrenderer.h"
#endif
#ifdef GDK_WINDOWING_MACOS
#include <gdk/macos/gdkmacos.h>
#endif
#ifdef GDK_WINDOWING_WIN32
#include <gdk/win32/gdkwin32.h>

/* Remove these lines when OpenGL/ES 2.0 shader is ready */
#include "win32/gdkprivate-win32.h"
#include "win32/gdkdisplay-win32.h"
#endif

typedef struct
{
  GObject parent_instance;

  GdkSurface *surface;
  GskRenderNode *prev_node;
  GskRenderNode *root_node;

  GskProfiler *profiler;

  GskDebugFlags debug_flags;

  unsigned int is_realized : 1;
} GskRendererPrivate;

G_DEFINE_ABSTRACT_TYPE_WITH_PRIVATE (GskRenderer, gsk_renderer, G_TYPE_OBJECT)

enum {
  PROP_0,
  PROP_REALIZED,
  PROP_SURFACE,

  N_PROPS
};

static GParamSpec *gsk_renderer_properties[N_PROPS];

#define GSK_RENDERER_WARN_NOT_IMPLEMENTED_METHOD(obj,method) \
  g_critical ("Renderer of type '%s' does not implement GskRenderer::" # method, G_OBJECT_TYPE_NAME (obj))

static gboolean
gsk_renderer_real_realize (GskRenderer  *self,
                           GdkSurface   *surface,
                           GError      **error)
{
  GSK_RENDERER_WARN_NOT_IMPLEMENTED_METHOD (self, realize);
  return FALSE;
}

static void
gsk_renderer_real_unrealize (GskRenderer *self)
{
  GSK_RENDERER_WARN_NOT_IMPLEMENTED_METHOD (self, unrealize);
}

static GdkTexture *
gsk_renderer_real_render_texture (GskRenderer           *self,
                                  GskRenderNode         *root,
                                  const graphene_rect_t *viewport)
{
  GSK_RENDERER_WARN_NOT_IMPLEMENTED_METHOD (self, render_texture);
  return NULL;
}

static void
gsk_renderer_real_render (GskRenderer          *self,
                          GskRenderNode        *root,
                          const cairo_region_t *region)
{
  GSK_RENDERER_WARN_NOT_IMPLEMENTED_METHOD (self, render);
}

static void
gsk_renderer_dispose (GObject *gobject)
{
  GskRenderer *self = GSK_RENDERER (gobject);
  GskRendererPrivate *priv = gsk_renderer_get_instance_private (self);

  /* We can't just unrealize here because superclasses have already run dispose.
   * So we insist that unrealize must be called before unreffing. */
  g_assert (!priv->is_realized);

  g_clear_object (&priv->profiler);

  G_OBJECT_CLASS (gsk_renderer_parent_class)->dispose (gobject);
}

static void
gsk_renderer_get_property (GObject    *gobject,
                           guint       prop_id,
                           GValue     *value,
                           GParamSpec *pspec)
{
  GskRenderer *self = GSK_RENDERER (gobject);
  GskRendererPrivate *priv = gsk_renderer_get_instance_private (self);

  switch (prop_id)
    {
    case PROP_REALIZED:
      g_value_set_boolean (value, priv->is_realized);
      break;

    case PROP_SURFACE:
      g_value_set_object (value, priv->surface);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (gobject, prop_id, pspec);
      break;
    }
}

static void
gsk_renderer_class_init (GskRendererClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  klass->realize = gsk_renderer_real_realize;
  klass->unrealize = gsk_renderer_real_unrealize;
  klass->render = gsk_renderer_real_render;
  klass->render_texture = gsk_renderer_real_render_texture;

  gobject_class->get_property = gsk_renderer_get_property;
  gobject_class->dispose = gsk_renderer_dispose;

  /**
   * GskRenderer:realized: (attributes org.gtk.Property.get=gsk_renderer_is_realized)
   *
   * Whether the renderer has been associated with a surface or draw context.
   */
  gsk_renderer_properties[PROP_REALIZED] =
    g_param_spec_boolean ("realized", NULL, NULL,
                          FALSE,
                          G_PARAM_READABLE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);

  /**
   * GskRenderer:surface: (attributes org.gtk.Property.get=gsk_renderer_get_surface)
   *
   * The surface associated with renderer.
   */
  gsk_renderer_properties[PROP_SURFACE] =
    g_param_spec_object ("surface", NULL, NULL,
                         GDK_TYPE_SURFACE,
                         G_PARAM_READABLE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (gobject_class, N_PROPS, gsk_renderer_properties);
}

static void
gsk_renderer_init (GskRenderer *self)
{
  GskRendererPrivate *priv = gsk_renderer_get_instance_private (self);

  priv->profiler = gsk_profiler_new ();
  priv->debug_flags = gsk_get_debug_flags ();
}

/**
 * gsk_renderer_get_surface: (attributes org.gtk.Method.get_property=surface)
 * @renderer: a `GskRenderer`
 *
 * Retrieves the `GdkSurface` set using gsk_enderer_realize().
 *
 * If the renderer has not been realized yet, %NULL will be returned.
 *
 * Returns: (transfer none) (nullable): a `GdkSurface`
 */
GdkSurface *
gsk_renderer_get_surface (GskRenderer *renderer)
{
  GskRendererPrivate *priv = gsk_renderer_get_instance_private (renderer);

  g_return_val_if_fail (GSK_IS_RENDERER (renderer), NULL);

  return priv->surface;
}

/**
 * gsk_renderer_is_realized: (attributes org.gtk.Method.get_property=realized)
 * @renderer: a `GskRenderer`
 *
 * Checks whether the @renderer is realized or not.
 *
 * Returns: %TRUE if the `GskRenderer` was realized, and %FALSE otherwise
 */
gboolean
gsk_renderer_is_realized (GskRenderer *renderer)
{
  GskRendererPrivate *priv = gsk_renderer_get_instance_private (renderer);

  g_return_val_if_fail (GSK_IS_RENDERER (renderer), FALSE);

  return priv->is_realized;
}

/**
 * gsk_renderer_realize:
 * @renderer: a `GskRenderer`
 * @surface: (nullable): the `GdkSurface` renderer will be used on
 * @error: return location for an error
 *
 * Creates the resources needed by the @renderer to render the scene
 * graph.
 *
 * Since GTK 4.6, the surface may be `NULL`, which allows using
 * renderers without having to create a surface.
 *
 * Note that it is mandatory to call [method@Gsk.Renderer.unrealize] before
 * destroying the renderer.
 *
 * Returns: Whether the renderer was successfully realized
 */
gboolean
gsk_renderer_realize (GskRenderer  *renderer,
                      GdkSurface   *surface,
                      GError      **error)
{
  GskRendererPrivate *priv = gsk_renderer_get_instance_private (renderer);

  g_return_val_if_fail (GSK_IS_RENDERER (renderer), FALSE);
  g_return_val_if_fail (!gsk_renderer_is_realized (renderer), FALSE);
  g_return_val_if_fail (surface == NULL || GDK_IS_SURFACE (surface), FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (surface)
    priv->surface = g_object_ref (surface);

  if (!GSK_RENDERER_GET_CLASS (renderer)->realize (renderer, surface, error))
    {
      g_clear_object (&priv->surface);
      return FALSE;
    }

  priv->is_realized = TRUE;

  g_object_notify (G_OBJECT (renderer), "realized");
  if (surface)
    g_object_notify (G_OBJECT (renderer), "surface");

  return TRUE;
}

/**
 * gsk_renderer_unrealize:
 * @renderer: a `GskRenderer`
 *
 * Releases all the resources created by gsk_renderer_realize().
 */
void
gsk_renderer_unrealize (GskRenderer *renderer)
{
  GskRendererPrivate *priv = gsk_renderer_get_instance_private (renderer);
  gboolean has_surface;

  g_return_if_fail (GSK_IS_RENDERER (renderer));

  if (!priv->is_realized)
    return;

  has_surface = priv->surface != NULL;

  GSK_RENDERER_GET_CLASS (renderer)->unrealize (renderer);

  g_clear_object (&priv->surface);
  g_clear_pointer (&priv->prev_node, gsk_render_node_unref);

  priv->is_realized = FALSE;

  g_object_notify (G_OBJECT (renderer), "realized");
  if (has_surface)
    g_object_notify (G_OBJECT (renderer), "surface");
}

/**
 * gsk_renderer_render_texture:
 * @renderer: a realized `GskRenderer`
 * @root: a `GskRenderNode`
 * @viewport: (nullable): the section to draw or %NULL to use @root's bounds
 *
 * Renders the scene graph, described by a tree of `GskRenderNode` instances,
 * to a `GdkTexture`.
 *
 * The @renderer will acquire a reference on the `GskRenderNode` tree while
 * the rendering is in progress.
 *
 * If you want to apply any transformations to @root, you should put it into a
 * transform node and pass that node instead.
 *
 * Returns: (transfer full): a `GdkTexture` with the rendered contents of @root.
 */
GdkTexture *
gsk_renderer_render_texture (GskRenderer           *renderer,
                             GskRenderNode         *root,
                             const graphene_rect_t *viewport)
{
  GskRendererPrivate *priv = gsk_renderer_get_instance_private (renderer);
  graphene_rect_t real_viewport;
  GdkTexture *texture;

  g_return_val_if_fail (GSK_IS_RENDERER (renderer), NULL);
  g_return_val_if_fail (priv->is_realized, NULL);
  g_return_val_if_fail (GSK_IS_RENDER_NODE (root), NULL);
  g_return_val_if_fail (priv->root_node == NULL, NULL);

  priv->root_node = gsk_render_node_ref (root);

  if (viewport == NULL)
    {
      gsk_render_node_get_bounds (root, &real_viewport);
      viewport = &real_viewport;
    }
  g_return_val_if_fail (viewport->size.width > 0, NULL);
  g_return_val_if_fail (viewport->size.height > 0, NULL);

  texture = GSK_RENDERER_GET_CLASS (renderer)->render_texture (renderer, root, viewport);

  if (GSK_RENDERER_DEBUG_CHECK (renderer, RENDERER))
    {
      GString *buf = g_string_new ("*** Texture stats ***\n\n");

      gsk_profiler_append_counters (priv->profiler, buf);
      g_string_append_c (buf, '\n');

      gsk_profiler_append_timers (priv->profiler, buf);
      g_string_append_c (buf, '\n');

      g_print ("%s\n***\n\n", buf->str);

      g_string_free (buf, TRUE);
    }

  g_clear_pointer (&priv->root_node, gsk_render_node_unref);

  return texture;
}

/**
 * gsk_renderer_render:
 * @renderer: a realized `GskRenderer`
 * @root: a `GskRenderNode`
 * @region: (nullable): the `cairo_region_t` that must be redrawn or %NULL
 *   for the whole window
 *
 * Renders the scene graph, described by a tree of `GskRenderNode` instances
 * to the renderer's surface,  ensuring that the given @region gets redrawn.
 *
 * If the renderer has no associated surface, this function does nothing.
 *
 * Renderers must ensure that changes of the contents given by the @root
 * node as well as the area given by @region are redrawn. They are however
 * free to not redraw any pixel outside of @region if they can guarantee that
 * it didn't change.
 *
 * The @renderer will acquire a reference on the `GskRenderNode` tree while
 * the rendering is in progress.
 */
void
gsk_renderer_render (GskRenderer          *renderer,
                     GskRenderNode        *root,
                     const cairo_region_t *region)
{
  GskRendererPrivate *priv = gsk_renderer_get_instance_private (renderer);
  cairo_region_t *clip;

  g_return_if_fail (GSK_IS_RENDERER (renderer));
  g_return_if_fail (priv->is_realized);
  g_return_if_fail (GSK_IS_RENDER_NODE (root));
  g_return_if_fail (priv->root_node == NULL);

  if (priv->surface == NULL)
    return;

  if (region == NULL || priv->prev_node == NULL || GSK_RENDERER_DEBUG_CHECK (renderer, FULL_REDRAW))
    {
      clip = cairo_region_create_rectangle (&(GdkRectangle) {
                                                0, 0,
                                                gdk_surface_get_width (priv->surface),
                                                gdk_surface_get_height (priv->surface)
                                            });
    }
  else
    {
      clip = cairo_region_copy (region);
      gsk_render_node_diff (priv->prev_node, root, clip);

      if (cairo_region_is_empty (clip))
        {
          cairo_region_destroy (clip);
          return;
        }
    }

  priv->root_node = gsk_render_node_ref (root);

  GSK_RENDERER_GET_CLASS (renderer)->render (renderer, root, clip);

  if (GSK_RENDERER_DEBUG_CHECK (renderer, RENDERER))
    {
      GString *buf = g_string_new ("*** Frame stats ***\n\n");

      gsk_profiler_append_counters (priv->profiler, buf);
      g_string_append_c (buf, '\n');

      gsk_profiler_append_timers (priv->profiler, buf);
      g_string_append_c (buf, '\n');

      g_print ("%s\n***\n\n", buf->str);

      g_string_free (buf, TRUE);
    }

  g_clear_pointer (&priv->prev_node, gsk_render_node_unref);
  cairo_region_destroy (clip);
  priv->prev_node = priv->root_node;
  priv->root_node = NULL;
}

/*< private >
 * gsk_renderer_get_profiler:
 * @renderer: a `GskRenderer`
 *
 * Retrieves a pointer to the `GskProfiler` instance of the renderer.
 *
 * Returns: (transfer none): the profiler
 */
GskProfiler *
gsk_renderer_get_profiler (GskRenderer *renderer)
{
  GskRendererPrivate *priv = gsk_renderer_get_instance_private (renderer);

  g_return_val_if_fail (GSK_IS_RENDERER (renderer), NULL);

  return priv->profiler;
}

static GType
get_renderer_for_name (const char *renderer_name)
{
  if (renderer_name == NULL)
    return G_TYPE_INVALID;
#ifdef GDK_WINDOWING_BROADWAY
  else if (g_ascii_strcasecmp (renderer_name, "broadway") == 0)
    return GSK_TYPE_BROADWAY_RENDERER;
#endif
  else if (g_ascii_strcasecmp (renderer_name, "cairo") == 0)
    return GSK_TYPE_CAIRO_RENDERER;
  else if (g_ascii_strcasecmp (renderer_name, "opengl") == 0 ||
           g_ascii_strcasecmp (renderer_name, "gl") == 0)
    return GSK_TYPE_GL_RENDERER;
#ifdef GDK_RENDERING_VULKAN
  else if (g_ascii_strcasecmp (renderer_name, "vulkan") == 0)
    return GSK_TYPE_VULKAN_RENDERER;
#endif
  else if (g_ascii_strcasecmp (renderer_name, "help") == 0)
    {
      g_print ("Supported arguments for GSK_RENDERER environment variable:\n");
#ifdef GDK_WINDOWING_BROADWAY
      g_print ("broadway - Use the Broadway specific renderer\n");
#else
      g_print ("broadway - Disabled during GTK build\n");
#endif
      g_print ("   cairo - Use the Cairo fallback renderer\n");
      g_print ("  opengl - Use the OpenGL renderer\n");
      g_print ("      gl - Use the OpenGL renderer\n");
#ifdef GDK_RENDERING_VULKAN
      g_print ("  vulkan - Use the Vulkan renderer\n");
#else
      g_print ("  vulkan - Disabled during GTK build\n");
#endif
      g_print ("    help - Print this help\n\n");
      g_print ("Other arguments will cause a warning and be ignored.\n");
    }
  else
    {
      g_warning ("Unrecognized renderer \"%s\". Try GSK_RENDERER=help", renderer_name);
    }

  return G_TYPE_INVALID;
}

static GType
get_renderer_for_display (GdkSurface *surface)
{
  GdkDisplay *display = gdk_surface_get_display (surface);
  const char *renderer_name;

  renderer_name = g_object_get_data (G_OBJECT (display), "gsk-renderer");
  return get_renderer_for_name (renderer_name);
}

static GType
get_renderer_for_env_var (GdkSurface *surface)
{
  static GType env_var_type = G_TYPE_NONE;

  if (env_var_type == G_TYPE_NONE)
    {
      const char *renderer_name = g_getenv ("GSK_RENDERER");
      env_var_type = get_renderer_for_name (renderer_name);
    }

  return env_var_type;
}

static GType
get_renderer_for_backend (GdkSurface *surface)
{
#ifdef GDK_WINDOWING_X11
  if (GDK_IS_X11_SURFACE (surface))
    return GSK_TYPE_GL_RENDERER;
#endif
#ifdef GDK_WINDOWING_WAYLAND
  if (GDK_IS_WAYLAND_SURFACE (surface))
    return GSK_TYPE_GL_RENDERER;
#endif
#ifdef GDK_WINDOWING_BROADWAY
  if (GDK_IS_BROADWAY_SURFACE (surface))
    return GSK_TYPE_BROADWAY_RENDERER;
#endif
#ifdef GDK_WINDOWING_MACOS
  if (GDK_IS_MACOS_SURFACE (surface))
    return GSK_TYPE_GL_RENDERER;
#endif
#ifdef GDK_WINDOWING_WIN32
  if (GDK_IS_WIN32_SURFACE (surface))
    return GSK_TYPE_GL_RENDERER;
#endif

  return G_TYPE_INVALID;
}

static GType
get_renderer_fallback (GdkSurface *surface)
{
  return GSK_TYPE_CAIRO_RENDERER;
}

static struct {
  GType (* get_renderer) (GdkSurface *surface);
} renderer_possibilities[] = {
  { get_renderer_for_display },
  { get_renderer_for_env_var },
  { get_renderer_for_backend },
  { get_renderer_fallback },
};

/**
 * gsk_renderer_new_for_surface:
 * @surface: a `GdkSurface`
 *
 * Creates an appropriate `GskRenderer` instance for the given @surface.
 *
 * If the `GSK_RENDERER` environment variable is set, GSK will
 * try that renderer first, before trying the backend-specific
 * default. The ultimate fallback is the cairo renderer.
 *
 * The renderer will be realized before it is returned.
 *
 * Returns: (transfer full) (nullable): a `GskRenderer`
 */
GskRenderer *
gsk_renderer_new_for_surface (GdkSurface *surface)
{
  GType renderer_type;
  GskRenderer *renderer;
  GError *error = NULL;
  guint i;

  g_return_val_if_fail (GDK_IS_SURFACE (surface), NULL);

  for (i = 0; i < G_N_ELEMENTS (renderer_possibilities); i++)
    {
      renderer_type = renderer_possibilities[i].get_renderer (surface);
      if (renderer_type == G_TYPE_INVALID)
        continue;

      renderer = g_object_new (renderer_type, NULL);

      if (gsk_renderer_realize (renderer, surface, &error))
        {
          GSK_RENDERER_DEBUG (renderer, RENDERER,
                              "Using renderer of type '%s' for surface '%s'",
                              G_OBJECT_TYPE_NAME (renderer),
                              G_OBJECT_TYPE_NAME (surface));
          return renderer;
        }

      g_message ("Failed to realize renderer of type '%s' for surface '%s': %s\n",
                 G_OBJECT_TYPE_NAME (renderer),
                 G_OBJECT_TYPE_NAME (surface),
                 error->message);
      g_object_unref (renderer);
      g_clear_error (&error);
    }

  g_assert_not_reached ();
  return NULL;
}

GskDebugFlags
gsk_renderer_get_debug_flags (GskRenderer *renderer)
{
  GskRendererPrivate *priv = gsk_renderer_get_instance_private (renderer);

  g_return_val_if_fail (GSK_IS_RENDERER (renderer), 0);

  return priv->debug_flags;
}

void
gsk_renderer_set_debug_flags (GskRenderer   *renderer,
                              GskDebugFlags  flags)
{
  GskRendererPrivate *priv = gsk_renderer_get_instance_private (renderer);

  g_return_if_fail (GSK_IS_RENDERER (renderer));

  priv->debug_flags = flags;
}

/* Feed a texture through a renderer and return the resulting 'native' texture. */
GdkTexture *
gsk_renderer_convert_texture (GskRenderer *self,
                              GdkTexture  *texture)
{
  int width, height;
  GskRenderNode *node;
  GdkTexture *result;

  width = gdk_texture_get_width (texture);
  height = gdk_texture_get_height (texture);

  node = gsk_texture_node_new (texture, &GRAPHENE_RECT_INIT (0, 0, width, height));
  result = gsk_renderer_render_texture (self, node, &GRAPHENE_RECT_INIT (0, 0, width, height));
  gsk_render_node_unref (node);

  return result;
}
