#include <Elementary.h>
#include "elm_priv.h"
#include "elm_widget_prefs.h"
#include "elm_prefs_edd.x"

#include "Eo.h"

EAPI Eo_Op ELM_OBJ_PREFS_BASE_ID = EO_NOOP;

#define MY_CLASS ELM_OBJ_PREFS_CLASS

#define MY_CLASS_NAME "elm_prefs"

static const char SIG_PAGE_CHANGED[] = "page,changed";
static const char SIG_PAGE_SAVED[] = "page,saved";
static const char SIG_PAGE_RESET[] = "page,reset";
static const char SIG_PAGE_LOADED[] = "page,loaded";
static const char SIG_ITEM_CHANGED[] = "item,changed";
static const char SIG_ACTION[] = "action";
static const Evas_Smart_Cb_Description _elm_prefs_smart_callbacks[] = {
   { SIG_PAGE_CHANGED, "s" },
   { SIG_PAGE_SAVED, "s" },
   { SIG_PAGE_RESET, "s" },
   { SIG_PAGE_LOADED, "s" },
   { SIG_ITEM_CHANGED, "s" },
   { SIG_ACTION, "ss" },
   { NULL, NULL}
};

static int _elm_prefs_init_count = 0;
static Eina_Hash *_elm_prefs_page_widgets_map = NULL;
static const Elm_Prefs_Page_Iface *_elm_prefs_page_default_widget = NULL;
static Eina_Hash *_elm_prefs_item_widgets_map = NULL;
static Eina_Hash *_elm_prefs_item_type_widgets_map = NULL;
static const Elm_Prefs_Item_Iface *_elm_prefs_item_default_widget = NULL;

static void _elm_prefs_values_get_default(Elm_Prefs_Page_Node *,
                                          Eina_Bool);
static Eina_Bool _prefs_item_widget_value_from_self(Elm_Prefs_Item_Node *,
                                                    Eina_Bool);

static void
_elm_prefs_smart_add(Eo *obj, void *_pd EINA_UNUSED, va_list *list EINA_UNUSED)
{
   eo_do_super(obj, evas_obj_smart_add());
}

static void _item_free(Elm_Prefs_Item_Node *it);

static void
_page_free(Elm_Prefs_Page_Node *p)
{
   Elm_Prefs_Item_Node *it;

   if (!p) return;

   eina_stringshare_del(p->name);
   eina_stringshare_del(p->title);
   eina_stringshare_del(p->sub_title);
   eina_stringshare_del(p->widget);
   eina_stringshare_del(p->style);
   eina_stringshare_del(p->icon);

   evas_object_del(p->w_obj);

   EINA_LIST_FREE(p->items, it)
      _item_free(it);

   free(p);
}

static void
_item_free(Elm_Prefs_Item_Node *it)
{
   switch (it->type)
     {
      case ELM_PREFS_TYPE_ACTION:
      case ELM_PREFS_TYPE_BOOL:
      case ELM_PREFS_TYPE_INT:
      case ELM_PREFS_TYPE_FLOAT:
      case ELM_PREFS_TYPE_LABEL:
      case ELM_PREFS_TYPE_DATE:
      case ELM_PREFS_TYPE_RESET:
      case ELM_PREFS_TYPE_SAVE:
      case ELM_PREFS_TYPE_SEPARATOR:
      case ELM_PREFS_TYPE_SWALLOW:
        break;

      case ELM_PREFS_TYPE_PAGE:
        eina_stringshare_del(it->spec.p.source);
        _page_free(it->subpage);
        it->w_obj = NULL;
        break;

      case ELM_PREFS_TYPE_TEXT:
      case ELM_PREFS_TYPE_TEXTAREA:
        eina_stringshare_del(it->spec.s.placeholder);
        eina_stringshare_del(it->spec.s.accept);
        eina_stringshare_del(it->spec.s.deny);
        break;

      default:
        ERR("bad item (type = %d), skipping it", it->type);
        break;
     }

   eina_stringshare_del(it->name);
   eina_stringshare_del(it->label);
   eina_stringshare_del(it->icon);
   eina_stringshare_del(it->style);
   eina_stringshare_del(it->widget);

   evas_object_del(it->w_obj); /* we have to delete them ourselves
                                * because of _prefs_item_del_cb() --
                                * it'll need the parent alive, to
                                * gather its smart data bit */
   free(it);
}

static Eina_Bool
_elm_prefs_save(void *data)
{
   ELM_PREFS_DATA_GET(data, sd);
   Elm_Widget_Smart_Data *wd = eo_data_get(data, ELM_OBJ_WIDGET_CLASS);

   if (!sd->dirty || !sd->prefs_data) goto end;

   if (!elm_prefs_data_autosave_get(sd->prefs_data))
     {
        elm_prefs_data_save(sd->prefs_data, NULL, NULL);

        evas_object_smart_callback_call
          (wd->obj, SIG_PAGE_SAVED, (char *)sd->root->name);
     }

   sd->dirty = EINA_FALSE;

end:
   sd->saving_poller = NULL;
   return ECORE_CALLBACK_CANCEL;
}

static void
_root_node_free(Elm_Prefs_Smart_Data *sd)
{
   _page_free(sd->root);
}

static Eina_Bool
_prefs_data_types_match(const Eina_Value_Type *t,
                        Elm_Prefs_Item_Type epd_t)
{
   return (t == EINA_VALUE_TYPE_UCHAR && epd_t == ELM_PREFS_TYPE_BOOL) ||
          (t == EINA_VALUE_TYPE_INT && epd_t == ELM_PREFS_TYPE_INT) ||
          (t == EINA_VALUE_TYPE_FLOAT && epd_t == ELM_PREFS_TYPE_FLOAT) ||
          (t == EINA_VALUE_TYPE_TIMEVAL && epd_t == ELM_PREFS_TYPE_DATE) ||
          (t == EINA_VALUE_TYPE_STRINGSHARE &&
           (epd_t == ELM_PREFS_TYPE_PAGE ||
            epd_t == ELM_PREFS_TYPE_TEXT ||
            epd_t == ELM_PREFS_TYPE_TEXTAREA));
}

static Eina_Bool
_prefs_data_type_fix(Elm_Prefs_Item_Node *it,
                     Eina_Value *value)
{
   Eina_Value v;
   Eina_Bool setup_err = EINA_FALSE;

   switch (it->type)
     {
      case ELM_PREFS_TYPE_BOOL:
        if (!eina_value_setup(&v, EINA_VALUE_TYPE_UCHAR))
          setup_err = EINA_TRUE;
        break;

      case ELM_PREFS_TYPE_INT:
        if (!eina_value_setup(&v, EINA_VALUE_TYPE_INT))
          setup_err = EINA_TRUE;
        break;

      case ELM_PREFS_TYPE_FLOAT:
        if (!eina_value_setup(&v, EINA_VALUE_TYPE_FLOAT))
          setup_err = EINA_TRUE;
        break;

      case ELM_PREFS_TYPE_DATE:
        if (!eina_value_setup(&v, EINA_VALUE_TYPE_TIMEVAL))
          setup_err = EINA_TRUE;
        break;

      case ELM_PREFS_TYPE_PAGE:
      case ELM_PREFS_TYPE_TEXT:
      case ELM_PREFS_TYPE_TEXTAREA:
        if (!eina_value_setup(&v, EINA_VALUE_TYPE_STRINGSHARE))
          setup_err = EINA_TRUE;
        break;

      default:
        ERR("bad item (type = %d) found, skipping it", it->type);
        return EINA_FALSE;
     }

   if (setup_err) return EINA_FALSE;

   if (!eina_value_convert(value, &v) || !eina_value_copy(&v, value))
     {
        ERR("problem converting mismatching item (%s) value",
            it->name);

        return EINA_FALSE;
     }

   eina_value_flush(&v);

   return EINA_TRUE;
}

static Eina_Bool
_prefs_item_widget_value_from_data(Elm_Prefs_Smart_Data *sd,
                                   Elm_Prefs_Item_Node *it,
                                   Eina_Value *value)
{
   char buf[PATH_MAX];
   const Eina_Value_Type *t = eina_value_type_get(value);

   if ((it->type <= ELM_PREFS_TYPE_UNKNOWN) ||
       (it->type > ELM_PREFS_TYPE_SWALLOW))
     {
        ERR("bad item (type = %d) found on page %s, skipping it",
            it->type, sd->page);

        return EINA_FALSE;
     }

   snprintf(buf, sizeof(buf), "%s:%s", it->page->name, it->name);

   if (!_prefs_data_types_match(t, it->type))
     {
        if (!_prefs_data_type_fix(it, value)) return EINA_FALSE;
        else
          {
             Eina_Bool v_set;

             sd->changing_from_ui = EINA_TRUE;

             v_set = elm_prefs_data_value_set(sd->prefs_data,
                                              buf, it->type, value);

             sd->changing_from_ui = EINA_FALSE;

             if (!v_set) return EINA_FALSE;
          }
     }

   if (!it->available)
     {
        ERR("widget of item %s has been deleted, we can't set values on it",
            it->name);
        return EINA_FALSE;
     }

   if (!it->w_impl->value_set(it->w_obj, value))
     {
        ERR("failed to set value on widget of item %s", it->name);
        return EINA_FALSE;
     }

   return EINA_TRUE;
}

static void
_elm_prefs_mark_as_dirty(Eo *obj)
{
   ELM_PREFS_DATA_GET(obj, sd);
   sd->dirty = EINA_TRUE;

   if (sd->autosave)
     {
        if (sd->saving_poller) return;

        sd->saving_poller = ecore_poller_add
            (ECORE_POLLER_CORE, 1, _elm_prefs_save, obj);
     }
}

static void
_elm_prefs_item_changed_report(Eo *obj,
                               Elm_Prefs_Item_Node *it)
{
   char buf[PATH_MAX];
   Elm_Widget_Smart_Data *wd = eo_data_get(obj, ELM_OBJ_WIDGET_CLASS);

   snprintf(buf, sizeof(buf), "%s:%s", it->page->name, it->name);

   evas_object_smart_callback_call
     (wd->obj, SIG_ITEM_CHANGED, buf);
}

static Elm_Prefs_Item_Node *
_elm_prefs_page_item_by_name(Elm_Prefs_Page_Node *p,
                             char **path)
{
   Elm_Prefs_Item_Node *it;
   Eina_List *l;
   char *token;

   token = strsep(path, ":");

   EINA_LIST_FOREACH(p->items, l, it)
     {
        if (strcmp(it->name, token)) continue;

        if (!*path)
          return it;
        else if (it->type == ELM_PREFS_TYPE_PAGE)
          return _elm_prefs_page_item_by_name(it->subpage, path);
     }

   return NULL;
}

static Elm_Prefs_Item_Node *
_elm_prefs_item_node_by_name(Elm_Prefs_Smart_Data *sd,
                             const char *name)
{
   char buf[PATH_MAX];
   char *token;
   char *aux = buf;

   strcpy(buf, name);

   token = strsep(&aux, ":");

   if (strcmp(sd->root->name, token))
     return NULL; // first token should be the current page name

   return _elm_prefs_page_item_by_name(sd->root, &aux);
}

static Eina_List *
_elm_prefs_item_list_node_by_name(Elm_Prefs_Smart_Data *sd,
                                  const char *name)
{
   Elm_Prefs_Item_Node *it;
   Eina_List *l;

   EINA_LIST_FOREACH(sd->root->items, l, it)
     if (!strcmp(it->name, name)) return l;

   return NULL;
}

static void
_prefs_data_item_changed_cb(void *cb_data,
                            Elm_Prefs_Data_Event_Type type __UNUSED__,
                            Elm_Prefs_Data *prefs_data,
                            void *event_info)
{
   Elm_Prefs_Data_Event_Changed *evt = event_info;
   Eo *obj = cb_data;
   Elm_Prefs_Item_Node *it;
   Eina_Value value;

   ELM_PREFS_DATA_GET(obj, sd);
   if (sd->changing_from_ui) return;

   it = _elm_prefs_item_node_by_name(sd, evt->key);
   if (!it) return;

   if (elm_prefs_data_value_get(prefs_data, evt->key, NULL, &value))
     {
        if (!_prefs_item_widget_value_from_data(sd, it, &value)) goto end;

        _elm_prefs_item_changed_report(obj, it);
        _elm_prefs_mark_as_dirty(obj);
     }
   else
     ERR("failed to fetch value from data after changed event");

end:
   eina_value_flush(&value);
   return;
}

static void
_prefs_data_autosaved_cb(void *cb_data,
                         Elm_Prefs_Data_Event_Type type __UNUSED__,
                         Elm_Prefs_Data *data __UNUSED__,
                         void *event_info)
{
   ELM_PREFS_DATA_GET(cb_data, sd);
   Elm_Widget_Smart_Data *wd = eo_data_get(cb_data, ELM_OBJ_WIDGET_CLASS);

   evas_object_smart_callback_call
     (wd->obj, SIG_PAGE_SAVED, event_info);

   sd->dirty = EINA_FALSE;
}

static Eina_Bool
_elm_prefs_data_cbs_add(Eo *obj,
                        Elm_Prefs_Data *prefs_data)
{
   if (!elm_prefs_data_event_callback_add
         (prefs_data, ELM_PREFS_DATA_EVENT_ITEM_CHANGED,
         _prefs_data_item_changed_cb, obj) ||
       !elm_prefs_data_event_callback_add
         (prefs_data, ELM_PREFS_DATA_EVENT_GROUP_AUTOSAVED,
         _prefs_data_autosaved_cb, obj))
     {
        ERR("error while adding item changed event callback to "
            "prefs data handle, keeping previous data");

        return EINA_FALSE;
     }

   return EINA_TRUE;
}

static void
_elm_prefs_data_cbs_del(Eo *obj)
{
   ELM_PREFS_DATA_GET(obj, sd);

   if (!sd->prefs_data) return;

   if (!elm_prefs_data_event_callback_del
         (sd->prefs_data, ELM_PREFS_DATA_EVENT_ITEM_CHANGED,
         _prefs_data_item_changed_cb, obj))
     ERR("error while removing item changed event callback from "
         "prefs data handle");

   if (!elm_prefs_data_event_callback_del
         (sd->prefs_data, ELM_PREFS_DATA_EVENT_GROUP_AUTOSAVED,
         _prefs_data_autosaved_cb, obj))
     ERR("error while removing page autosave event callback from "
         "prefs data handle");
}

static void
_elm_prefs_smart_del(Eo *obj, void *_pd, va_list *list EINA_UNUSED)
{
   Elm_Prefs_Smart_Data *sd = _pd;

   sd->on_deletion = EINA_TRUE;

   if (sd->saving_poller) ecore_poller_del(sd->saving_poller);

   _elm_prefs_data_cbs_del(obj);

   if (sd->root)
     {
        elm_prefs_data_version_set(sd->prefs_data, sd->root->version);

        _elm_prefs_save(obj);

        _root_node_free(sd);
     }

   if (sd->prefs_data) elm_prefs_data_unref(sd->prefs_data);

   eina_stringshare_del(sd->file);
   eina_stringshare_del(sd->page);

   eo_do_super(obj, evas_obj_smart_del());
}

static void
_elm_prefs_smart_focus_next(Eo *obj, void *_pd, va_list *list)
{
   Elm_Prefs_Smart_Data *sd = _pd;
   Elm_Focus_Direction dir = va_arg(*list, Elm_Focus_Direction);
   Evas_Object **next = va_arg(*list, Evas_Object **);
   Eina_Bool *ret = va_arg(*list, Eina_Bool *);
   Eina_Bool mret;

   const Eina_List *items;

   ELM_PREFS_CHECK(obj);

   items = elm_widget_focus_custom_chain_get(obj);
   if (items)
     {
        mret = elm_widget_focus_list_next_get
           (obj, items, eina_list_data_get, dir, next);

        if (ret) *ret = mret;
        return;
     }

   if (sd->root && sd->root->w_obj)
     {
        mret = elm_widget_focus_next_get(sd->root->w_obj, dir, next);
        if (ret) *ret = mret;
        return;
     }

   if (next) *next = NULL;

   if (ret) *ret = EINA_FALSE;
}

EAPI Evas_Object *
elm_prefs_add(Evas_Object *parent)
{
   EINA_SAFETY_ON_NULL_RETURN_VAL(parent, NULL);

   if (!_elm_prefs_init_count)
     {
        ERR("prefs_iface module is not loaded! you can't"
            " create prefs widgets");
        return NULL;
     }

   Evas_Object *obj = eo_add(MY_CLASS, parent);
   eo_unref(obj);
   return obj;
}

static void
_constructor(Eo *obj, void *_pd EINA_UNUSED, va_list *list EINA_UNUSED)
{
   eo_do_super(obj, eo_constructor());
   eo_do(obj,
         evas_obj_type_set(MY_CLASS_NAME),
         evas_obj_smart_callbacks_descriptions_set(_elm_prefs_smart_callbacks,
                                                   NULL));

   Evas_Object *parent = eo_parent_get(obj);

   if (!elm_widget_sub_object_add(parent, obj))
     ERR("could not add %p as sub object of %p", obj, parent);
}

static Eina_Bool
_elm_prefs_item_has_value(Elm_Prefs_Item_Node *it)
{
   return (it->type != ELM_PREFS_TYPE_ACTION) &&
          (it->type != ELM_PREFS_TYPE_LABEL) &&
          (it->type != ELM_PREFS_TYPE_RESET) &&
          (it->type != ELM_PREFS_TYPE_SAVE) &&
          (it->type != ELM_PREFS_TYPE_PAGE) &&
          (it->type != ELM_PREFS_TYPE_SEPARATOR) &&
          (it->type != ELM_PREFS_TYPE_SWALLOW);
}

static void
_item_changed_cb(Evas_Object *it_obj)
{
   char buf[PATH_MAX];
   Elm_Prefs_Item_Node *it = evas_object_data_get(it_obj, "prefs_item");

   /* some widgets mark themselves changed as early as in their add()
    * interface methods */
   if (!it) return;

   snprintf(buf, sizeof(buf), "%s:%s", it->page->name, it->name);

   ELM_PREFS_DATA_GET(it->prefs, sd);
   Elm_Widget_Smart_Data *wd = eo_data_get(it->prefs, ELM_OBJ_WIDGET_CLASS);

   if (sd->values_fetching) goto end;

   /* we use the changed cb on ACTION/RESET/SAVE items specially */
   if (it->type == ELM_PREFS_TYPE_ACTION)
     {
        evas_object_smart_callback_call
          (wd->obj, SIG_ACTION, buf);

        return;
     }
   else if (it->type == ELM_PREFS_TYPE_RESET)
     {
        _elm_prefs_values_get_default(sd->root, EINA_TRUE);
        _elm_prefs_mark_as_dirty(it->prefs);

        return;
     }
   else if (it->type == ELM_PREFS_TYPE_SAVE)
     {
        if (sd->saving_poller) return;

        sd->saving_poller = ecore_poller_add
            (ECORE_POLLER_CORE, 1, _elm_prefs_save, it->prefs);

        return;
     }

   if (!it->persistent || !_elm_prefs_item_has_value(it)) return;

   if (it->w_impl->value_validate &&
       !it->w_impl->value_validate(it->w_obj))
     {
        if (sd->prefs_data)
          {
             Eina_Value value;

             // Restoring to the last valid value.
             if (!elm_prefs_data_value_get(sd->prefs_data, buf, NULL, &value))
               goto restore_fail;
             if (!it->w_impl->value_set(it->w_obj, &value))
               {
                  eina_value_flush(&value);
                  goto restore_fail;
               }
          }
        else
          {
             if (!_prefs_item_widget_value_from_self(it, EINA_FALSE))
               goto restore_fail;
          }

        return;
     }

end:
   if (sd->prefs_data)
     {
        Eina_Value value;

        if (!it->w_impl->value_get(it->w_obj, &value))
          ERR("failed to fetch value from widget of item %s", buf);
        else
          {
             sd->changing_from_ui = EINA_TRUE;
             elm_prefs_data_value_set(sd->prefs_data, buf, it->type, &value);
             eina_value_flush(&value);
             sd->changing_from_ui = EINA_FALSE;
          }
     }

   if (!sd->values_fetching) _elm_prefs_item_changed_report(it->prefs, it);

   _elm_prefs_mark_as_dirty(it->prefs);

   return;

restore_fail:
   ERR("failed to restore the last valid value from widget of item %s",
       buf);
}

static Eina_Bool
_prefs_item_widget_value_from_self(Elm_Prefs_Item_Node *it,
                                   Eina_Bool mark_changed)
{
   Eina_Value value;

   if (!_elm_prefs_item_has_value(it)) return EINA_TRUE;

   switch (it->type)
     {
      case ELM_PREFS_TYPE_BOOL:
        if (!eina_value_setup(&value, EINA_VALUE_TYPE_UCHAR)) goto err;
        if (!eina_value_set(&value, it->spec.b.def))
          {
             eina_value_flush(&value);
             goto err;
          }
        break;

      case ELM_PREFS_TYPE_INT:
        if (!eina_value_setup(&value, EINA_VALUE_TYPE_INT)) goto err;
        if (!eina_value_set(&value, it->spec.i.def))
          {
             eina_value_flush(&value);
             goto err;
          }
        break;

      case ELM_PREFS_TYPE_FLOAT:
        if (!eina_value_setup(&value, EINA_VALUE_TYPE_FLOAT)) goto err;
        if (!eina_value_set(&value, it->spec.f.def))
          {
             eina_value_flush(&value);
             goto err;
          }
        break;

      case ELM_PREFS_TYPE_DATE:
      {
         struct timeval val = {0};
         struct tm time = {0};

         time.tm_year = it->spec.d.def.y - 1900;
         time.tm_mon = it->spec.d.def.m - 1;
         time.tm_mday = it->spec.d.def.d;
         val.tv_sec = mktime(&time);

         if (!eina_value_setup(&value, EINA_VALUE_TYPE_TIMEVAL)) goto err;
         if (!eina_value_set(&value, val))
           {
              eina_value_flush(&value);
              goto err;
           }
      }
      break;

      case ELM_PREFS_TYPE_TEXT:
      case ELM_PREFS_TYPE_TEXTAREA:
        if (!eina_value_setup(&value, EINA_VALUE_TYPE_STRINGSHARE)) goto err;
        if (!eina_value_set(&value, it->spec.s.placeholder))
          {
             eina_value_flush(&value);
             goto err;
          }
        break;

      case ELM_PREFS_TYPE_PAGE:
      case ELM_PREFS_TYPE_SEPARATOR: //page is the value setter for separators
      case ELM_PREFS_TYPE_SWALLOW: //prefs is the value setter for swallows
        return EINA_TRUE;

      default:
        ERR("bad item (type = %d) found, skipping it", it->type);

        return EINA_FALSE;
     }

   if (!it->available)
     {
        ERR("widget of item %s has been deleted, we can't set values on it",
            it->name);

        eina_value_flush(&value);
        return EINA_FALSE;
     }

   if (!it->w_impl->value_set(it->w_obj, &value)) goto err;
   else
     {
        if (mark_changed) _item_changed_cb(it->w_obj);
        eina_value_flush(&value);
        return EINA_TRUE;
     }

err:
   ERR("failed to set value on widget of item %s", it->name);

   return EINA_FALSE;
}

static Eina_Bool
_elm_prefs_page_widget_new(Evas_Object *obj,
                           Elm_Prefs_Page_Node *page)
{
   page->parent = obj;

   if (page->widget)
     {
        page->w_impl =
          eina_hash_find(_elm_prefs_page_widgets_map, page->widget);
        if (!page->w_impl)
          {
             ERR("widget %s is not eligible to implement page %s,"
                 "trying default widget for pages",
                 page->widget, page->name);
          }
        else goto wid_found;
     }
   else
     INF("no explicit widget declared for page %s,"
         " trying default widget for pages", page->name);

   page->w_impl = _elm_prefs_page_default_widget;
   ERR("no widget bound to this page, fallbacking to default widget");

wid_found:

   page->w_obj = page->w_impl->add(page->w_impl, obj);
   if (!page->w_obj)
     {
        ERR("error while adding UI element to prefs widget %p", obj);
        return EINA_FALSE;
     }

   if (page->title && page->w_impl->title_set)
     {
        if (!page->w_impl->title_set(page->w_obj, page->title))
          {
             ERR("failed to set title %s on page %s",
                 page->title, page->name);
          }
     }

   if (page->sub_title && page->w_impl->sub_title_set)
     {
        if (!page->w_impl->sub_title_set(page->w_obj, page->sub_title))
          {
             ERR("failed to set sub_title %s on page %s",
                 page->sub_title, page->name);
          }
     }

   if (page->icon && page->w_impl->icon_set)
     {
        if (!page->w_impl->icon_set(page->w_obj, page->icon))
          {
             ERR("failed to set icon %s on page %s",
                 page->icon, page->name);
          }
     }

   evas_object_data_set(page->w_obj, "prefs_page", page);

   evas_object_show(page->w_obj);

   return EINA_TRUE;
}

static void
_elm_prefs_item_external_label_inject(Elm_Prefs_Item_Node *it)
{
   Evas_Object *label = elm_label_add(it->w_obj);
   elm_layout_text_set(label, NULL, it->label);
   evas_object_data_set(it->w_obj, "label_widget", label);
   evas_object_show(label);
}

static void
_elm_prefs_item_external_icon_inject(Elm_Prefs_Item_Node *it)
{
   Evas_Object *icon = elm_icon_add(it->w_obj);
   elm_icon_standard_set(icon, it->icon);
   elm_image_resizable_set(icon, EINA_FALSE, EINA_FALSE);
   evas_object_data_set(it->w_obj, "icon_widget", icon);
   evas_object_show(icon);
}

static void
_elm_prefs_item_properties_apply(Elm_Prefs_Item_Node *item)
{
   if (item->label)
     {
        if (item->w_impl->label_set)
          {
             if (!item->w_impl->label_set(item->w_obj, item->label))
               {
                  ERR("failed to set label %s on item %s through widget"
                      " implementation method, fallbacking to page's "
                      "method", item->label, item->name);

                  _elm_prefs_item_external_label_inject(item);
               }
          }
        else
          _elm_prefs_item_external_label_inject(item);
     }

   if (item->icon)
     {
        if (item->w_impl->icon_set)
          {
             if (!item->w_impl->icon_set(item->w_obj, item->icon))
               {
                  ERR("failed to set icon %s on item %s through widget"
                      " implementation method, fallbacking to page's "
                      "method", item->label, item->name);

                  _elm_prefs_item_external_icon_inject(item);
               }
          }
        else
          _elm_prefs_item_external_icon_inject(item);
     }

   if (item->w_impl->editable_set &&
       !item->w_impl->editable_set(item->w_obj, item->editable))
     {
        ERR("failed to set editability (%d) on item %s",
            item->editable, item->name);
     }

   if (item->style && !elm_object_style_set(item->w_obj, item->style))
     {
        ERR("failed to set style %s on item %s",
            item->style, item->name);
     }
}

static Eina_Bool
_elm_prefs_item_widget_new(Evas_Object *obj,
                           Elm_Prefs_Page_Node *parent,
                           Elm_Prefs_Item_Node *item)
{
   item->prefs = obj;
   item->available = EINA_TRUE;
   item->page = parent;

   if (item->widget)
     {
        item->w_impl =
          eina_hash_find(_elm_prefs_item_widgets_map, item->widget);
        if (!item->w_impl)
          {
             ERR("widget %s is not eligible to implement item of"
                 " type %d, trying default widget for that type",
                 item->widget, item->type);
          }
        else goto wid_found;
     }
   else
     INF("no explicit item widget declared for %s,"
         " trying default widget for its type (%d)", item->name, item->type);

   item->w_impl =
     eina_hash_find(_elm_prefs_item_type_widgets_map, &item->type);

   if (!item->w_impl)
     {
        item->w_impl = _elm_prefs_item_default_widget;
        ERR("no widget bound to this type, fallbacking to "
            "default widget");
     }

wid_found:

   item->w_obj = item->w_impl->add(
       item->w_impl, obj, item->type, item->spec, _item_changed_cb);
   if (!item->w_obj)
     {
        ERR("error while adding UI element to prefs widget %p", obj);
        return EINA_FALSE;
     }

   evas_object_data_set(item->w_obj, "prefs_item", item);

   _elm_prefs_item_properties_apply(item);

   evas_object_show(item->w_obj);

   return EINA_TRUE;
}

static Elm_Prefs_Page_Node *
_elm_prefs_page_load(Evas_Object *obj,
                     const char  *pname)
{
   Eet_File *eet_file;
   Elm_Prefs_Page_Node *ret = NULL;

   ELM_PREFS_CHECK(obj) NULL;
   EINA_SAFETY_ON_NULL_RETURN_VAL(pname, NULL);

   ELM_PREFS_DATA_GET(obj, sd);

   eet_file = eet_open(sd->file, EET_FILE_MODE_READ);

   if (eet_file)
     {
        ret = eet_data_read(eet_file, _page_edd, pname);
        eet_close(eet_file);

        if (!ret)
          ERR("problem while reading from file %s, key %s", sd->file, pname);

        ret->prefs = obj;
     }
   else
     ERR("failed to load from requested epb file (%s)", sd->file);

   return ret;
}

static Eina_Bool
_elm_prefs_page_populate(Elm_Prefs_Page_Node *page,
                         Evas_Object         *parent)
{
   Elm_Prefs_Page_Node *subpage;
   Elm_Prefs_Item_Node *it;
   Eina_List           *l;

   EINA_SAFETY_ON_NULL_RETURN_VAL(page, EINA_FALSE);

   if (!_elm_prefs_page_widget_new(parent, page)) goto err;

   EINA_LIST_FOREACH(page->items, l, it)
     {
        if ((it->type <= ELM_PREFS_TYPE_UNKNOWN) ||
            (it->type > ELM_PREFS_TYPE_SWALLOW))
          {
             ERR("bad item (type = %d) found on page %s, skipping it",
                 it->type, page->name);
             continue;
          }
        else if (it->type == ELM_PREFS_TYPE_PAGE)
          {
             subpage = _elm_prefs_page_load(page->prefs, it->spec.p.source);
             if (!subpage)
               {
                  ERR("subpage %s could not be created inside %s, skipping it",
                      it->name, page->name);
                  continue;
               }

             eina_stringshare_del(subpage->name);
             subpage->name = eina_stringshare_printf("%s:%s",
                                                     page->name, it->name);

             if (!_elm_prefs_page_populate(subpage, page->w_obj))
               {
                  _page_free(subpage);
                  goto err;
               }

             it->prefs     = page->prefs;
             it->page      = page;
             it->w_obj     = subpage->w_obj;
             it->w_impl    = NULL;
             it->available = EINA_TRUE;
             it->subpage   = subpage;
          }
        else if (!_elm_prefs_item_widget_new(page->prefs, page, it)) goto err;

        if (it->visible && !page->w_impl->item_pack
            (page->w_obj, it->w_obj, it->type, it->w_impl))
          {
             ERR("item %s could not be packed inside page %s",
                 it->name, page->name);

             goto err;
          }
     }

   return EINA_TRUE;

err:
   EINA_LIST_FOREACH(page->items, l, it)
     {
        if (it->w_obj) evas_object_del(it->w_obj);
        it->w_obj = NULL;
        it->w_impl = NULL;
     }

   if (page->w_obj) evas_object_del(page->w_obj);
   page->w_obj = NULL;
   page->w_impl = NULL;

   return EINA_FALSE;
}

static void
_elm_prefs_values_get_default(Elm_Prefs_Page_Node *page,
                              Eina_Bool mark_changed)
{
   Eina_List *l;
   Elm_Prefs_Item_Node *it;

   EINA_LIST_FOREACH(page->items, l, it)
     {
        if (it->type == ELM_PREFS_TYPE_PAGE)
          _elm_prefs_values_get_default(it->subpage, mark_changed);
        else
          _prefs_item_widget_value_from_self(it, mark_changed);
     }
}

static void
_elm_prefs_values_get_user(Elm_Prefs_Smart_Data *sd,
                           Elm_Prefs_Page_Node *p)
{
   char buf[PATH_MAX];
   Eina_List *l;
   Eina_Value value;
   Elm_Prefs_Item_Node *it;

   if (!sd->file) return;

   EINA_LIST_FOREACH(p->items, l, it)
     {
        Eina_Bool get_err = EINA_FALSE, set_err = EINA_FALSE;

        if (it->type == ELM_PREFS_TYPE_PAGE)
          {
             Elm_Prefs_Page_Node *subp = it->subpage;

             if (!elm_prefs_data_value_get
                 (sd->prefs_data, subp->name, NULL, &value))
               {
                  INF("failed to fetch value for item %s on user data, "
                      "writing UI value back on it", it->name);

                  if (eina_value_setup(&value, EINA_VALUE_TYPE_STRINGSHARE) &&
                      eina_value_set(&value, subp->name))
                    {
                       sd->changing_from_ui = EINA_TRUE;
                       elm_prefs_data_value_set
                          (sd->prefs_data, subp->name, it->type, &value);
                       sd->changing_from_ui = EINA_FALSE;
                    }
               }

             _elm_prefs_values_get_user(sd, subp);

             eina_value_flush(&value);
             continue;
          }

        if (!_elm_prefs_item_has_value(it)) continue;
        if (!it->persistent) continue;

        snprintf(buf, sizeof(buf), "%s:%s", p->name, it->name);

        if (!elm_prefs_data_value_get(sd->prefs_data, buf, NULL, &value))
          get_err = EINA_TRUE;
        else if (!_prefs_item_widget_value_from_data(sd, it, &value))
          set_err = EINA_TRUE;

        if (get_err || set_err)
          {
             if (get_err)
               INF("failed to fetch value for item %s on user data, "
                   "writing UI value back on it", it->name);

             /* force writing back our default value for it */
             if (it->available)
               {
                  if (!it->w_impl->value_get(it->w_obj, &value))
                    ERR("failed to fetch value from widget of item %s",
                        it->name);
                  else
                    {
                       sd->changing_from_ui = EINA_TRUE;
                       elm_prefs_data_value_set
                          (sd->prefs_data, buf, it->type, &value);
                       sd->changing_from_ui = EINA_FALSE;
                    }
               }
          }

        eina_value_flush(&value);
     }
}

EAPI Eina_Bool
elm_prefs_file_set(Evas_Object *obj,
                   const char *file,
                   const char *page)
{
   ELM_PREFS_CHECK(obj) EINA_FALSE;
   Eina_Bool ret;
   eo_do(obj, elm_obj_prefs_file_set(file, page, &ret));
   return ret;
}

static void
_elm_prefs_file_set(Eo *obj, void *_pd, va_list *list)
{
   const char *prefix;
   const char *file = va_arg(*list, const char *);
   const char *page = va_arg(*list, const char *);
   Eina_Bool  *ret  = va_arg(*list, Eina_Bool *);

   Elm_Prefs_Smart_Data *sd = _pd;

   prefix = elm_app_data_dir_get();
   if (!strlen(prefix))
     {
        WRN("we could not figure out the program's data"
            " dir, fallbacking to local directory.");
        prefix = ".";
     }

   if (!file)
     sd->file = eina_stringshare_printf("%s/%s", prefix, "preferences.epb");
   else
     {
        if (*file != '/') /* relative */
          sd->file = eina_stringshare_printf("%s/%s", prefix, file);
        else
          sd->file = eina_stringshare_add(file);
     }

   sd->page = eina_stringshare_add(page ? page : "main");

   sd->root = _elm_prefs_page_load(obj, sd->page);
   if (!sd->root)
     {
        *ret = EINA_FALSE;
        return;
     }

   if (!_elm_prefs_page_populate(sd->root, obj))
     {
        _root_node_free(sd);
        sd->root = NULL;

        *ret = EINA_FALSE;
        return;
     }

   elm_widget_resize_object_set(obj, sd->root->w_obj);

   _elm_prefs_values_get_default(sd->root, EINA_FALSE);

   evas_object_smart_callback_call
      (obj, SIG_PAGE_LOADED, (char *)sd->root->name);

   *ret = EINA_TRUE;
}

EAPI Eina_Bool
elm_prefs_file_get(const Evas_Object *obj,
                   const char **file,
                   const char **page)
{
   ELM_PREFS_CHECK(obj) EINA_FALSE;
   Eina_Bool ret;
   eo_do((Eo *) obj, elm_obj_prefs_file_get(file, page, &ret));
   return ret;
}

static void
_elm_prefs_file_get(Eo *obj EINA_UNUSED, void *_pd, va_list *list)
{
   const char **file = va_arg(*list, const char **);
   const char **page = va_arg(*list, const char **);
   Eina_Bool *ret = va_arg(*list, Eina_Bool *);
   Elm_Prefs_Smart_Data *sd = _pd;

   if (file) *file = sd->file;
   if (page) *page = sd->page;

   *ret = EINA_TRUE;
}

EAPI Eina_Bool
elm_prefs_data_set(Evas_Object *obj,
                   Elm_Prefs_Data *prefs_data)
{
   ELM_PREFS_CHECK(obj) EINA_FALSE;
   Eina_Bool ret;
   eo_do(obj, elm_obj_prefs_data_set(prefs_data, &ret));
   return ret;
}

static void
_elm_prefs_data_set(Eo *obj, void *_pd, va_list *list)
{
   Elm_Prefs_Smart_Data *sd = _pd;
   Elm_Prefs_Data *prefs_data = va_arg(*list, Elm_Prefs_Data *);
   Eina_Bool *ret = va_arg(*list, Eina_Bool *);

   if (!sd->root)
     {
        *ret = EINA_FALSE;
        return;
     }

   if (prefs_data && !_elm_prefs_data_cbs_add(obj, prefs_data))
     {
        *ret = EINA_FALSE;
        return;
     }

   if (sd->prefs_data)
     {
        _elm_prefs_data_cbs_del(obj);

        elm_prefs_data_unref(sd->prefs_data);
     }

   sd->prefs_data = prefs_data;

   if (!sd->prefs_data)
     {
        INF("resetting prefs to default values");
        _elm_prefs_values_get_default(sd->root, EINA_FALSE);

        *ret = EINA_TRUE;
        return;
     }

   elm_prefs_data_ref(sd->prefs_data);

   sd->values_fetching = EINA_TRUE;
   _elm_prefs_values_get_user(sd, sd->root);
   sd->values_fetching = EINA_FALSE;

   *ret = EINA_TRUE;
}

EAPI Elm_Prefs_Data *
elm_prefs_data_get(const Evas_Object *obj)
{
   ELM_PREFS_CHECK(obj) NULL;
   Elm_Prefs_Data *ret;
   eo_do((Eo *) obj, elm_obj_prefs_data_get(&ret));
   return ret;
}

static void
_elm_prefs_data_get(Eo *obj EINA_UNUSED, void *_pd, va_list *list)
{
   Elm_Prefs_Smart_Data *sd = _pd;
   Elm_Prefs_Data **ret = va_arg(*list, Elm_Prefs_Data **);

   if (!sd->root) *ret = NULL;
   else *ret = sd->prefs_data;
}

EAPI void
elm_prefs_autosave_set(Evas_Object *obj,
                       Eina_Bool autosave)
{
   ELM_PREFS_CHECK(obj);
   eo_do(obj, elm_obj_prefs_autosave_set(autosave));
}

static void
_elm_prefs_autosave_set(Eo *obj, void *_pd EINA_UNUSED, va_list *list)
{
   ELM_PREFS_DATA_GET(obj, sd);
   Eina_Bool autosave = va_arg(*list, int);

   autosave = !!autosave;

   if (sd->autosave != autosave)
     sd->autosave = autosave;
   else
     return;

   if ((sd->autosave) && (sd->dirty))
     {
        if (sd->saving_poller) return;

        sd->saving_poller = ecore_poller_add
            (ECORE_POLLER_CORE, 1, _elm_prefs_save, obj);
     }
   else if ((!sd->autosave) && (sd->saving_poller))
     {
        ecore_poller_del(sd->saving_poller);
        sd->saving_poller = NULL;

        _elm_prefs_save(obj);
     }
}

EAPI Eina_Bool
elm_prefs_autosave_get(const Evas_Object *obj)
{
   ELM_PREFS_CHECK(obj) EINA_FALSE;
   Eina_Bool ret;
   eo_do((Eo *) obj, elm_obj_prefs_autosave_get(&ret));
   return ret;
}

static void
_elm_prefs_autosave_get(Eo *obj EINA_UNUSED, void *_pd, va_list *list)
{
   Elm_Prefs_Smart_Data *sd = _pd;
   Eina_Bool *ret = va_arg(*list, Eina_Bool *);

   *ret = sd->autosave;
}

EAPI void
elm_prefs_reset(Evas_Object *obj,
                Elm_Prefs_Reset_Mode mode)
{
   ELM_PREFS_CHECK(obj);
   eo_do(obj, elm_obj_prefs_reset(mode));
}

static void
_elm_prefs_reset(Eo *obj EINA_UNUSED, void *_pd, va_list *list)
{
   Elm_Prefs_Smart_Data *sd = _pd;
   Elm_Prefs_Reset_Mode mode = va_arg(*list, Elm_Prefs_Reset_Mode);

   EINA_SAFETY_ON_NULL_RETURN(sd->root);

   if (mode == ELM_PREFS_RESET_DEFAULTS)
     _elm_prefs_values_get_default(sd->root, EINA_TRUE);
   else if (mode == ELM_PREFS_RESET_LAST)
     WRN("ELM_PREFS_RESET_LAST not implemented yet");
}

static Elm_Prefs_Item_Node *
_elm_prefs_item_api_entry_common(const Evas_Object *obj,
                                 const char *it_name)
{
   Elm_Prefs_Item_Node *ret;

   ELM_PREFS_CHECK(obj) EINA_FALSE;
   ELM_PREFS_DATA_GET(obj, sd);

   EINA_SAFETY_ON_NULL_RETURN_VAL(it_name, EINA_FALSE);

   EINA_SAFETY_ON_NULL_RETURN_VAL(sd->root, EINA_FALSE);

   ret = _elm_prefs_item_node_by_name(sd, it_name);

   if (!ret) ERR("item with name %s does not exist on file %s",
                 it_name, sd->file);

   return ret;
}

EAPI Eina_Bool
elm_prefs_item_value_set(Evas_Object *obj,
                         const char *name,
                         const Eina_Value *value)
{
   ELM_PREFS_CHECK(obj) EINA_FALSE;
   Eina_Bool ret;
   eo_do(obj, elm_obj_prefs_item_value_set(name, value, &ret));
   return ret;
}

static void
_elm_prefs_item_value_set(Eo *obj, void *_pd EINA_UNUSED, va_list *list)
{
   const Eina_Value_Type *t, *def_t;
   Elm_Prefs_Item_Node *it;
   Eina_Value it_val;

   const char *name = va_arg(*list, const char *);
   const Eina_Value *value = va_arg(*list, const Eina_Value *);
   Eina_Bool *ret = va_arg(*list, Eina_Bool *);

   it = _elm_prefs_item_api_entry_common(obj, name);
   if (!it)
     {
        *ret = EINA_FALSE;
        return;
     }

   if (!_elm_prefs_item_has_value(it))
     {
        ERR("item %s has no underlying value, you can't operate on it",
            it->name);
        *ret = EINA_FALSE;
        return;
     }

   EINA_SAFETY_ON_NULL_RETURN(value);
   t = eina_value_type_get(value);
   if (!t)
     {
        *ret = EINA_FALSE;
        return;
     }

   if (!it->available)
     {
        ERR("widget of item %s has been deleted, we can't set values on it",
            it->name);
        *ret = EINA_FALSE;
        return;
     }

   if (!it->w_impl->value_get(it->w_obj, &it_val))
     {
        ERR("failed to fetch value from widget of item %s", it->name);
        goto err;
     }

   def_t = eina_value_type_get(&it_val);
   if ((t != def_t) && (!eina_value_convert(value, &it_val)))
     {
        eina_value_flush(&it_val);
        ERR("failed to convert value to proper type");
        goto err;
     }
   else if (!eina_value_copy(value, &it_val) ||
            (!it->w_impl->value_set(it->w_obj, &it_val)))
     {
        eina_value_flush(&it_val);
        ERR("failed to set value on widget of item %s", it->name);
        goto err;
     }

   eina_value_flush(&it_val);
   *ret = EINA_TRUE;
   return;

err:
   *ret = EINA_FALSE;
}

EAPI Eina_Bool
elm_prefs_item_value_get(const Evas_Object *obj,
                         const char *name,
                         Eina_Value *value)
{
   ELM_PREFS_CHECK(obj) EINA_FALSE;
   Eina_Bool ret;
   eo_do((Eo *) obj, elm_obj_prefs_item_value_get(name, value, &ret));
   return ret;
}

static void
_elm_prefs_item_value_get(Eo *obj, void *_pd EINA_UNUSED, va_list *list)
{
   Elm_Prefs_Item_Node *it;

   const char *name = va_arg(*list, const char *);
   const Eina_Value *value = va_arg(*list, const Eina_Value *);
   Eina_Bool *ret = va_arg(*list, Eina_Bool *);

   it = _elm_prefs_item_api_entry_common(obj, name);
   if (!it)
     {
        *ret = EINA_FALSE;
        return;
     }

   if (!_elm_prefs_item_has_value(it))
     {
        ERR("item %s has no underlying value, you can't operate on it",
            it->name);
        *ret = EINA_FALSE;
        return;
     }

   if (!value)
     {
        *ret = EINA_FALSE;
        return;
     }

   if (!it->available)
     {
        ERR("widget of item %s has been deleted, we can't set values on it",
            it->name);
        *ret = EINA_FALSE;
        return;
     }

   if (!it->w_impl->value_get(it->w_obj, value))
     {
        ERR("failed to fetch value from widget of item %s", it->name);
        *ret = EINA_FALSE;
        return;
     }

   *ret = EINA_TRUE;
}

EAPI const Evas_Object *
elm_prefs_item_object_get(Evas_Object *obj,
                          const char *name)
{
   ELM_PREFS_CHECK(obj) NULL;
   const Evas_Object *ret;
   eo_do(obj, elm_obj_prefs_item_object_get(name, &ret));
   return ret;
}

static void
_elm_prefs_item_object_get(Eo *obj, void *_pd EINA_UNUSED, va_list *list)
{
   Elm_Prefs_Item_Node *it;
   const char *name = va_arg(*list, const char *);
   const Evas_Object **ret = va_arg(*list, const Evas_Object **);

   it = _elm_prefs_item_api_entry_common(obj, name);
   if (!it) *ret = NULL;

   *ret = it->w_obj;
}

EAPI void
elm_prefs_item_visible_set(Evas_Object *obj,
                           const char *name,
                           Eina_Bool visible)
{
   ELM_PREFS_CHECK(obj);
   eo_do(obj, elm_obj_prefs_item_visible_set(name, visible));
}

static void
_elm_prefs_item_visible_set(Eo *obj EINA_UNUSED, void *_pd, va_list *list)
{
   Elm_Prefs_Item_Node *it;
   Eina_List *l;
   Evas_Object *lbl, *icon;

   Elm_Prefs_Smart_Data *sd = _pd;
   const char *name = va_arg(*list, const char *);
   Eina_Bool visible = va_arg(*list, int);

   EINA_SAFETY_ON_NULL_RETURN(name);
   EINA_SAFETY_ON_NULL_RETURN(sd->root);

   l = _elm_prefs_item_list_node_by_name(sd, name);
   if (!l) return;

   it = eina_list_data_get(l);

   visible = !!visible;

   if (it->visible == visible) return;
   it->visible = visible;


   if (!it->available)
     {
        ERR("widget of item %s has been deleted, we can't act on it",
            it->name);
        return;
     }

   lbl = evas_object_data_get(it->w_obj, "label_widget");
   icon = evas_object_data_get(it->w_obj, "icon_widget");

   if (!it->visible)
     {
        if (!it->page->w_impl->item_unpack(it->page->w_obj, it->w_obj))
          {
             ERR("failed to unpack item %s from page %s!",
                 it->name, it->page->name);
          }
        else
          {
             if (lbl) evas_object_hide(lbl);
             if (icon) evas_object_hide(icon);
             evas_object_hide(it->w_obj);
          }
     }
   else if (it->available)
     {
        Eina_List *p_l;

        if ((p_l = eina_list_prev(l)))
          {
             Elm_Prefs_Item_Node *p_it = eina_list_data_get(p_l);

             if (!it->page->w_impl->item_pack_after
                   (it->page->w_obj, it->w_obj,
                    p_it->w_obj, it->type, it->w_impl))
               {
                  ERR("failed to pack item %s on page %s!",
                      it->name, it->page->name);
               }
          }
        else if ((p_l = eina_list_next(l)))
          {
             Elm_Prefs_Item_Node *n_it = eina_list_data_get(p_l);

             if (!it->page->w_impl->item_pack_before
                   (it->page->w_obj, it->w_obj,
                    n_it->w_obj, it->type, it->w_impl))
               {
                  ERR("failed to pack item %s on page %s!",
                      it->name, it->page->name);
               }
          }
        else if (!it->page->w_impl->item_pack
                   (it->page->w_obj, it->w_obj, it->type, it->w_impl))
          ERR("failed to pack item %s on page %s!", it->name, it->page->name);

        if (lbl) evas_object_show(lbl);
        if (icon) evas_object_show(icon);
        evas_object_show(it->w_obj);
     }
}

EAPI Eina_Bool
elm_prefs_item_visible_get(const Evas_Object *obj,
                           const char *name)
{
   ELM_PREFS_CHECK(obj) EINA_FALSE;
   Eina_Bool ret;
   eo_do((Eo *) obj, elm_obj_prefs_item_visible_get(name, &ret));
   return ret;
}

static void
_elm_prefs_item_visible_get(Eo *obj, void *_pd EINA_UNUSED, va_list *list)
{
   const char *name = va_arg(*list, const char *);
   Eina_Bool *ret = va_arg(*list, Eina_Bool *);
   Elm_Prefs_Item_Node *it;

   it = _elm_prefs_item_api_entry_common(obj, name);
   if (!it)
     {
        *ret = EINA_FALSE;
        return;
     }

   if (!it->available)
     {
        ERR("widget of item %s has been deleted, we can't act on it",
            it->name);
        *ret = EINA_FALSE;
        return;
     }

   *ret = it->visible;
}

EAPI void
elm_prefs_item_disabled_set(Evas_Object *obj,
                            const char *name,
                            Eina_Bool disabled)
{
   ELM_PREFS_CHECK(obj);
   eo_do(obj, elm_obj_prefs_item_disabled_set(name, disabled));
}

static void
_elm_prefs_item_disabled_set(Eo *obj, void *_pd EINA_UNUSED, va_list *list)
{
   const char *name = va_arg(*list, const char *);
   Eina_Bool disabled = va_arg(*list, int);
   Elm_Prefs_Item_Node *it;

   it = _elm_prefs_item_api_entry_common(obj, name);
   if (!it) return;

   if (!it->available)
     {
        ERR("widget of item %s has been deleted, we can't act on it",
            it->name);
        return;
     }

   elm_object_disabled_set(it->w_obj, disabled);
}

EAPI Eina_Bool
elm_prefs_item_disabled_get(const Evas_Object *obj,
                            const char *name)
{
   ELM_PREFS_CHECK(obj) EINA_FALSE;
   Eina_Bool ret;
   eo_do((Eo *) obj, elm_obj_prefs_item_disabled_get(name, &ret));
   return ret;
}

static void
_elm_prefs_item_disabled_get(Eo *obj, void *_pd EINA_UNUSED, va_list *list)
{
   const char *name = va_arg(*list, const char *);
   Eina_Bool *ret = va_arg(*list, Eina_Bool *);
   Elm_Prefs_Item_Node *it;

   it = _elm_prefs_item_api_entry_common(obj, name);
   if (!it)
     {
        *ret = EINA_FALSE;
        return;
     }

   if (!it->available)
     {
        ERR("widget of item %s has been deleted, we can't act on it",
            it->name);
        *ret = EINA_FALSE;
        return;
     }

   *ret = elm_object_disabled_get(it->w_obj);
}

EAPI void
elm_prefs_item_editable_set(Evas_Object *obj,
                            const char *name,
                            Eina_Bool editable)
{
   ELM_PREFS_CHECK(obj);
   eo_do(obj, elm_obj_prefs_item_editable_set(name, editable));
}

static void
_elm_prefs_item_editable_set(Eo *obj, void *_pd EINA_UNUSED, va_list *list)
{
   const char *name = va_arg(*list, const char *);
   Eina_Bool editable = va_arg(*list, int);
   Elm_Prefs_Item_Node *it;

   it = _elm_prefs_item_api_entry_common(obj, name);
   if (!it) return;

   if (!it->w_impl->editable_set)
     {
        ERR("the item %s does not implement the 'editable' "
            "property (using widget %s)", it->name, it->widget);
        return;
     }

   it->w_impl->editable_set(it->w_obj, editable);
}

EAPI Eina_Bool
elm_prefs_item_editable_get(const Evas_Object *obj,
                            const char *name)
{
   ELM_PREFS_CHECK(obj) EINA_FALSE;
   Eina_Bool ret;
   eo_do((Eo *) obj, elm_obj_prefs_item_editable_get(name, &ret));
   return ret;
}

static void
_elm_prefs_item_editable_get(Eo *obj, void *_pd EINA_UNUSED, va_list *list)
{
   const char *name = va_arg(*list, const char *);
   Eina_Bool *ret = va_arg(*list, Eina_Bool *);
   Elm_Prefs_Item_Node *it;

   it = _elm_prefs_item_api_entry_common(obj, name);
   if (!it)
     {
        *ret = EINA_FALSE;
        return;
     }

   if (!it->w_impl->editable_get)
     {
        ERR("the item %s does not implement the 'editable' "
            "property (using widget %s)", it->name, it->widget);
        *ret = EINA_FALSE;
        return;
     }

   *ret = it->w_impl->editable_get(it->w_obj);
}

EAPI Eina_Bool
elm_prefs_item_swallow(Evas_Object *obj,
                       const char *name,
                       Evas_Object *child)
{
   ELM_PREFS_CHECK(obj) EINA_FALSE;
   Eina_Bool ret;
   eo_do(obj, elm_obj_prefs_item_swallow(name, child, &ret));
   return ret;
}

static void
_elm_prefs_item_swallow(Eo *obj, void *_pd EINA_UNUSED, va_list *list)
{
   Eina_Value v;
   const char *name = va_arg(*list, const char *);
   Evas_Object *child = va_arg(*list, Evas_Object *);
   Eina_Bool *ret = va_arg(*list, Eina_Bool *);

   Elm_Prefs_Item_Node *it = _elm_prefs_item_api_entry_common(obj, name);
   if (!it)
     {
        *ret = EINA_FALSE;
        return;
     }

   if (it->type != ELM_PREFS_TYPE_SWALLOW)
     {
        ERR("item %s does not match a SWALLOW item", name);
        *ret = EINA_FALSE;
        return;
     }

   if (!eina_value_setup(&v, EINA_VALUE_TYPE_UINT64))
     {
        *ret = EINA_FALSE;
        return;
     }
   if (!eina_value_set(&v, child))
     {
        eina_value_flush(&v);
        *ret = EINA_FALSE;
        return;
     }

   *ret = it->w_impl->value_set(it->w_obj, &v);
   eina_value_flush(&v);
}

EAPI Evas_Object *
elm_prefs_item_unswallow(Evas_Object *obj,
                         const char *name)
{
   ELM_PREFS_CHECK(obj) NULL;
   Evas_Object *ret;
   eo_do(obj, elm_obj_prefs_item_unswallow(name, &ret));
   return ret;
}

static void
_elm_prefs_item_unswallow(Eo *obj, void *_pd EINA_UNUSED, va_list *list)
{
   Eina_Value v;
   const char *name = va_arg(*list, const char *);
   Evas_Object **ret = va_arg(*list, Evas_Object **);

   Elm_Prefs_Item_Node *it = _elm_prefs_item_api_entry_common(obj, name);
   if (!it)
     {
        *ret = NULL;
        return;
     }

   if (it->type != ELM_PREFS_TYPE_SWALLOW)
     {
        ERR("item %s does not match a SWALLOW item", name);
        *ret = NULL;
        return;
     }

   if (!(it->w_impl->value_get(it->w_obj, &v)))
     {
        *ret = NULL;
        return;
     }

   if (eina_value_type_get(&v) != EINA_VALUE_TYPE_UINT64 ||
       !eina_value_get(&v, ret))
     {
        eina_value_flush(&v);
        *ret = NULL;
        return;
     }

   eina_value_flush(&v);
}

static unsigned int
elm_prefs_item_iface_abi_version_get(void)
{
   return ELM_PREFS_ITEM_IFACE_ABI_VERSION;
}

EAPI void
elm_prefs_item_iface_register(const Elm_Prefs_Item_Iface_Info *array)
{
   const Elm_Prefs_Item_Iface_Info *itr;
   unsigned int abi_version = elm_prefs_item_iface_abi_version_get();

   if (!array)
     return;

   for (itr = array; itr->widget_name; itr++)
     {
        const Elm_Prefs_Item_Type *t_itr;

        if (itr->info->abi_version != abi_version)
          {
             ERR("external prefs widget interface '%s' (%p) has incorrect ABI "
                 "version. got %#x where %#x was expected.",
                 itr->widget_name, itr->info, itr->info->abi_version,
                 abi_version);
             continue;
          }

        /* FIXME: registering the 1st, for now */
        if (!_elm_prefs_item_default_widget)
          _elm_prefs_item_default_widget = itr->info;

        eina_hash_direct_add
          (_elm_prefs_item_widgets_map, itr->widget_name, itr->info);

        for (t_itr = itr->info->types;
             *t_itr != ELM_PREFS_TYPE_UNKNOWN; t_itr++)
          eina_hash_add(_elm_prefs_item_type_widgets_map, t_itr, itr->info);
     }
}

EAPI void
elm_prefs_item_iface_unregister(const Elm_Prefs_Item_Iface_Info *array)
{
   const Elm_Prefs_Item_Iface_Info *itr;

   if (!array)
     return;

   for (itr = array; itr->widget_name; itr++)
     {
        const Elm_Prefs_Item_Type *t_itr;

        eina_hash_del(_elm_prefs_item_widgets_map, itr->widget_name, itr->info);

        for (t_itr = itr->info->types;
             *t_itr != ELM_PREFS_TYPE_UNKNOWN; t_itr++)
          eina_hash_del
            (_elm_prefs_item_type_widgets_map, t_itr, itr->info);
     }
}

static unsigned int
elm_prefs_page_iface_abi_version_get(void)
{
   return ELM_PREFS_PAGE_IFACE_ABI_VERSION;
}

EAPI void
elm_prefs_page_iface_register(const Elm_Prefs_Page_Iface_Info *array)
{
   const Elm_Prefs_Page_Iface_Info *itr;
   unsigned int abi_version = elm_prefs_page_iface_abi_version_get();

   if (!array)
     return;

   for (itr = array; itr->widget_name; itr++)
     {
        if (itr->info->abi_version != abi_version)
          {
             ERR("external prefs widget interface '%s' (%p) has incorrect ABI "
                 "version. got %#x where %#x was expected.",
                 itr->widget_name, itr->info, itr->info->abi_version,
                 abi_version);
             continue;
          }

        /* FIXME: registering the 1st, for now */
        if (!_elm_prefs_page_default_widget)
          _elm_prefs_page_default_widget = itr->info;

        eina_hash_direct_add
          (_elm_prefs_page_widgets_map, itr->widget_name, itr->info);
     }
}

EAPI void
elm_prefs_page_iface_unregister(const Elm_Prefs_Page_Iface_Info *array)
{
   const Elm_Prefs_Page_Iface_Info *itr;

   if (!array)
     return;

   for (itr = array; itr->widget_name; itr++)
     eina_hash_del
       (_elm_prefs_page_widgets_map, itr->widget_name, itr->info);
}

/* TODO: evaluate if it's sane to handle it better */
/* static void */
/* _prefs_page_del_cb(void *data __UNUSED__, */
/*                    Evas *evas __UNUSED__, */
/*                    Evas_Object *obj, */
/*                    void *event_info __UNUSED__) */
/* { */
/*    Elm_Prefs_Page_Node *page; */

/*    evas_object_event_callback_del(obj, EVAS_CALLBACK_DEL, _prefs_page_del_cb); */

/*    page = evas_object_data_get(obj, "prefs_page"); */

/*    ELM_PREFS_DATA_GET(page->prefs, sd); */

/*    if (!sd->on_deletion) */
/*      { */
/*          Eina_List *l; */
/*          Elm_Prefs_Item_Node *it; */

/*         /\* force writing back the value for it *\/ */
/*         EINA_LIST_FOREACH(page->items, l, it) */
/*           _item_changed_cb(it->w_obj); */
/*      } */

/*    evas_object_data_del(obj, "prefs_page"); */
/*    page->w_obj = NULL; */
/* } */

static void
_prefs_item_del_cb(void *data __UNUSED__,
                   Evas *evas __UNUSED__,
                   Evas_Object *obj,
                   void *event_info __UNUSED__)
{
   Elm_Prefs_Item_Node *it;
   Evas_Object *lbl, *icon;

   evas_object_event_callback_del(obj, EVAS_CALLBACK_DEL, _prefs_item_del_cb);

   it = evas_object_data_get(obj, "prefs_item");
   lbl = evas_object_data_del(it->w_obj, "label_widget");
   if (lbl) evas_object_del(lbl);

   icon = evas_object_data_del(it->w_obj, "icon_widget");
   if (icon) evas_object_del(icon);

   ELM_PREFS_DATA_GET(it->prefs, sd);

   if (!sd->on_deletion)
     /* force writing back the value for it */
     _item_changed_cb(obj);

   evas_object_data_del(obj, "prefs_item");
   it->available = EINA_FALSE;

   it->w_obj = NULL;
}

EAPI Eina_Bool
elm_prefs_item_widget_common_add(Evas_Object *prefs __UNUSED__,
                                 Evas_Object *obj)
{
   EINA_SAFETY_ON_NULL_RETURN_VAL(obj, EINA_FALSE);

   evas_object_event_callback_add
     (obj, EVAS_CALLBACK_DEL, _prefs_item_del_cb, NULL);

   return EINA_TRUE;
}

EAPI Eina_Bool
elm_prefs_page_widget_common_add(Evas_Object *prefs __UNUSED__,
                                 Evas_Object *obj)
{
   EINA_SAFETY_ON_NULL_RETURN_VAL(obj, EINA_FALSE);

   /* evas_object_event_callback_add */
   /*   (obj, EVAS_CALLBACK_DEL, _prefs_page_del_cb, NULL); */

   return EINA_TRUE;
}

void
_elm_prefs_init(void)
{
   Elm_Module *m;

   if (++_elm_prefs_init_count != 1)
     return;

   _elm_prefs_descriptors_init();
   _elm_prefs_data_init();

   _elm_prefs_page_widgets_map = eina_hash_string_superfast_new(NULL);
   _elm_prefs_item_widgets_map = eina_hash_string_superfast_new(NULL);
   _elm_prefs_item_type_widgets_map = eina_hash_int32_new(NULL);

   if (!(m = _elm_module_find_as("prefs_iface")))
     {
        ERR("prefs iface module could not be loaded,"
            " the prefs widget won't function");

        return;
     }
   m->init_func(m);
}

void
_elm_prefs_shutdown(void)
{
   if (_elm_prefs_init_count <= 0)
     {
        EINA_LOG_ERR("Init count not greater than 0 in shutdown.");
        return;
     }
   if (--_elm_prefs_init_count != 0) return;

   _elm_prefs_descriptors_shutdown();
   _elm_prefs_data_shutdown();

   eina_hash_free(_elm_prefs_page_widgets_map);
   eina_hash_free(_elm_prefs_item_widgets_map);
   eina_hash_free(_elm_prefs_item_type_widgets_map);

   /* all modules shutdown calls will taken place elsewhere */
}

static void
_class_constructor(Eo_Class *klass)
{
   const Eo_Op_Func_Description func_desc[] = {
        EO_OP_FUNC(EO_BASE_ID(EO_BASE_SUB_ID_CONSTRUCTOR), _constructor),

        EO_OP_FUNC(EVAS_OBJ_SMART_ID(EVAS_OBJ_SMART_SUB_ID_ADD),
                   _elm_prefs_smart_add),
        EO_OP_FUNC(EVAS_OBJ_SMART_ID(EVAS_OBJ_SMART_SUB_ID_DEL),
                   _elm_prefs_smart_del),

        EO_OP_FUNC(ELM_WIDGET_ID(ELM_WIDGET_SUB_ID_FOCUS_NEXT),
                   _elm_prefs_smart_focus_next),

        EO_OP_FUNC(ELM_OBJ_PREFS_ID(ELM_OBJ_PREFS_SUB_ID_FILE_SET),
                   _elm_prefs_file_set),
        EO_OP_FUNC(ELM_OBJ_PREFS_ID(ELM_OBJ_PREFS_SUB_ID_FILE_GET),
                   _elm_prefs_file_get),
        EO_OP_FUNC(ELM_OBJ_PREFS_ID(ELM_OBJ_PREFS_SUB_ID_DATA_SET),
                   _elm_prefs_data_set),
        EO_OP_FUNC(ELM_OBJ_PREFS_ID(ELM_OBJ_PREFS_SUB_ID_DATA_GET),
                   _elm_prefs_data_get),
        EO_OP_FUNC(ELM_OBJ_PREFS_ID(ELM_OBJ_PREFS_SUB_ID_AUTOSAVE_SET),
                   _elm_prefs_autosave_set),
        EO_OP_FUNC(ELM_OBJ_PREFS_ID(ELM_OBJ_PREFS_SUB_ID_AUTOSAVE_GET),
                   _elm_prefs_autosave_get),
        EO_OP_FUNC(ELM_OBJ_PREFS_ID(ELM_OBJ_PREFS_SUB_ID_RESET),
                   _elm_prefs_reset),
        EO_OP_FUNC(ELM_OBJ_PREFS_ID(ELM_OBJ_PREFS_SUB_ID_ITEM_VALUE_SET),
                   _elm_prefs_item_value_set),
        EO_OP_FUNC(ELM_OBJ_PREFS_ID(ELM_OBJ_PREFS_SUB_ID_ITEM_VALUE_GET),
                   _elm_prefs_item_value_get),
        EO_OP_FUNC(ELM_OBJ_PREFS_ID(ELM_OBJ_PREFS_SUB_ID_ITEM_OBJECT_GET),
                   _elm_prefs_item_object_get),
        EO_OP_FUNC(ELM_OBJ_PREFS_ID(ELM_OBJ_PREFS_SUB_ID_ITEM_VISIBLE_SET),
                   _elm_prefs_item_visible_set),
        EO_OP_FUNC(ELM_OBJ_PREFS_ID(ELM_OBJ_PREFS_SUB_ID_ITEM_VISIBLE_GET),
                   _elm_prefs_item_visible_get),
        EO_OP_FUNC(ELM_OBJ_PREFS_ID(ELM_OBJ_PREFS_SUB_ID_ITEM_DISABLED_SET),
                   _elm_prefs_item_disabled_set),
        EO_OP_FUNC(ELM_OBJ_PREFS_ID(ELM_OBJ_PREFS_SUB_ID_ITEM_DISABLED_GET),
                   _elm_prefs_item_disabled_get),
        EO_OP_FUNC(ELM_OBJ_PREFS_ID(ELM_OBJ_PREFS_SUB_ID_ITEM_EDITABLE_SET),
                   _elm_prefs_item_editable_set),
        EO_OP_FUNC(ELM_OBJ_PREFS_ID(ELM_OBJ_PREFS_SUB_ID_ITEM_EDITABLE_GET),
                   _elm_prefs_item_editable_get),
        EO_OP_FUNC(ELM_OBJ_PREFS_ID(ELM_OBJ_PREFS_SUB_ID_ITEM_SWALLOW),
                   _elm_prefs_item_swallow),
        EO_OP_FUNC(ELM_OBJ_PREFS_ID(ELM_OBJ_PREFS_SUB_ID_ITEM_UNSWALLOW),
                   _elm_prefs_item_unswallow),
        EO_OP_FUNC_SENTINEL
   };
   eo_class_funcs_set(klass, func_desc);
}

static const Eo_Op_Description op_desc[] = {
     EO_OP_DESCRIPTION
     (ELM_OBJ_PREFS_SUB_ID_FILE_SET, "Set file and page to populate a given "
      "prefs widget's interface."),
     EO_OP_DESCRIPTION
     (ELM_OBJ_PREFS_SUB_ID_FILE_GET, "Retrieve file and page bound to a given "
      "prefs widget."),
     EO_OP_DESCRIPTION
     (ELM_OBJ_PREFS_SUB_ID_DATA_SET, "Set user data for a given prefs widget."),
     EO_OP_DESCRIPTION
     (ELM_OBJ_PREFS_SUB_ID_DATA_GET, "Retrieve user data for a given prefs "
      "widget."),
     EO_OP_DESCRIPTION
     (ELM_OBJ_PREFS_SUB_ID_AUTOSAVE_SET, "Set whether a given prefs widget "
      "should save its values back (on the user data file, if set) "
      "automatically on every UI element changes."),
     EO_OP_DESCRIPTION
     (ELM_OBJ_PREFS_SUB_ID_AUTOSAVE_GET, "Get whether a given prefs widget is "
      "saving its values back automatically on changes."),
     EO_OP_DESCRIPTION
     (ELM_OBJ_PREFS_SUB_ID_RESET, "Reset the values of a given prefs widget to "
      "a previous state."),
     EO_OP_DESCRIPTION
     (ELM_OBJ_PREFS_SUB_ID_ITEM_VALUE_SET, "Set the value on a given prefs "
      "widget's item."),
     EO_OP_DESCRIPTION
     (ELM_OBJ_PREFS_SUB_ID_ITEM_VALUE_GET, "Retrieve the value of a given prefs"
      " widget's item."),
     EO_OP_DESCRIPTION
     (ELM_OBJ_PREFS_SUB_ID_ITEM_OBJECT_GET, "Retrieve the Elementary widget "
      "bound to a given prefs widget's item."),
     EO_OP_DESCRIPTION
     (ELM_OBJ_PREFS_SUB_ID_ITEM_VISIBLE_SET, "Set whether the widget bound to "
      "given prefs widget's item should be visible or not."),
     EO_OP_DESCRIPTION
     (ELM_OBJ_PREFS_SUB_ID_ITEM_VISIBLE_GET, "Retrieve whether the widget bound"
      " to a given prefs widget's item is visible or not."),
     EO_OP_DESCRIPTION
     (ELM_OBJ_PREFS_SUB_ID_ITEM_DISABLED_SET, "Set whether the widget bound to "
      "a given prefs widget's item is disabled or not."),
     EO_OP_DESCRIPTION
     (ELM_OBJ_PREFS_SUB_ID_ITEM_DISABLED_GET, "Retrieve whether the widget "
      "bound to a given prefs widget's item is disabled or not."),
     EO_OP_DESCRIPTION
     (ELM_OBJ_PREFS_SUB_ID_ITEM_EDITABLE_SET, "Set whether the widget bound to "
      "a given prefs widget's item is editable or not."),
     EO_OP_DESCRIPTION
     (ELM_OBJ_PREFS_SUB_ID_ITEM_EDITABLE_GET, "Retrieve whether the widget "
      "bound to a given prefs widget's item is editable or not."),
     EO_OP_DESCRIPTION
     (ELM_OBJ_PREFS_SUB_ID_ITEM_SWALLOW, "\"Swallows\" an object into a SWALLOW"
      " item of a prefs widget."),
     EO_OP_DESCRIPTION
     (ELM_OBJ_PREFS_SUB_ID_ITEM_UNSWALLOW, "Unswallow an object from a SWALLOW"
      " item of a prefs widget."),
     EO_OP_DESCRIPTION_SENTINEL
};

static const Eo_Class_Description class_desc = {
     EO_VERSION,
     MY_CLASS_NAME,
     EO_CLASS_TYPE_REGULAR,
     EO_CLASS_DESCRIPTION_OPS
     (&ELM_OBJ_PREFS_BASE_ID, op_desc, ELM_OBJ_PREFS_SUB_ID_LAST),
     NULL,
     sizeof(Elm_Prefs_Smart_Data),
     _class_constructor,
     NULL
};

EO_DEFINE_CLASS(elm_obj_prefs_class_get, &class_desc, ELM_OBJ_WIDGET_CLASS, NULL);