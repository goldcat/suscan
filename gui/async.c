/*

  Copyright (C) 2017 Gonzalo José Carracedo Carballal

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as
  published by the Free Software Foundation, either version 3 of the
  License, or (at your option) any later version.

  This program is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with this program.  If not, see
  <http://www.gnu.org/licenses/>

*/
#include <assert.h>

#define SU_LOG_DOMAIN "async-thread"

#include "gui.h"

/* Asynchronous thread: take messages from analyzer and parse them */
struct suscan_gui_msg_envelope {
  struct suscan_gui *gui;
  uint32_t type;
  void *private;
};

void
suscan_gui_msg_envelope_destroy(struct suscan_gui_msg_envelope *data)
{
  suscan_analyzer_dispose_message(
      data->type,
      data->private);

  free(data);
}

struct suscan_gui_msg_envelope *
suscan_gui_msg_envelope_new(
    struct suscan_gui *gui,
    uint32_t type,
    void *private)
{
  struct suscan_gui_msg_envelope *new;

  SU_TRYCATCH(
      new = malloc(sizeof (struct suscan_gui_msg_envelope)),
      return NULL);

  new->gui = gui;
  new->private = private;
  new->type = type;

  return new;
}

/************************** Update GUI state *********************************/
void
suscan_gui_change_button_icon(GtkButton *button, const char *icon)
{
  GtkWidget *prev;
  GtkWidget *image;

  SU_TRYCATCH(
      image = gtk_image_new_from_icon_name(icon, GTK_ICON_SIZE_BUTTON),
      return);

  prev = gtk_bin_get_child(GTK_BIN(button));
  gtk_container_remove(GTK_CONTAINER(button), prev);
  gtk_widget_show(GTK_WIDGET(image));
  gtk_container_add(GTK_CONTAINER(button), image);
}

void
suscan_gui_update_state(struct suscan_gui *gui, enum suscan_gui_state state)
{
  const char *source_name = "No source selected";
  char *subtitle = NULL;

  if (gui->selected_config != NULL)
    source_name = gui->selected_config->source->desc;

  switch (state) {
    case SUSCAN_GUI_STATE_STOPPED:
      subtitle = strbuild("%s (Stopped)", source_name);
      suscan_gui_change_button_icon(
          GTK_BUTTON(gui->toggleConnect),
          "media-playback-start-symbolic");
      gtk_widget_set_sensitive(GTK_WIDGET(gui->toggleConnect), TRUE);
      gtk_widget_set_sensitive(GTK_WIDGET(gui->preferencesButton), TRUE);
      gtk_widget_set_sensitive(GTK_WIDGET(gui->openInspectorMenuItem), FALSE);
      gtk_widget_set_sensitive(GTK_WIDGET(gui->recentMenu), TRUE);
      break;

    case SUSCAN_GUI_STATE_RUNNING:
      subtitle = strbuild("%s (Running)", source_name);
      suscan_gui_change_button_icon(
          GTK_BUTTON(gui->toggleConnect),
          "media-playback-stop-symbolic");
      gtk_widget_set_sensitive(GTK_WIDGET(gui->toggleConnect), TRUE);
      gtk_widget_set_sensitive(GTK_WIDGET(gui->preferencesButton), FALSE);
      gtk_widget_set_sensitive(GTK_WIDGET(gui->openInspectorMenuItem), TRUE);
      gtk_widget_set_sensitive(GTK_WIDGET(gui->recentMenu), FALSE);
      break;

    case SUSCAN_GUI_STATE_STOPPING:
    case SUSCAN_GUI_STATE_QUITTING:
      subtitle = strbuild("%s (Stopping...)", source_name);
      suscan_gui_change_button_icon(
          GTK_BUTTON(gui->toggleConnect),
          "media-playback-start-symbolic");
      gtk_widget_set_sensitive(GTK_WIDGET(gui->toggleConnect), FALSE);
      gtk_widget_set_sensitive(GTK_WIDGET(gui->preferencesButton), FALSE);
      gtk_widget_set_sensitive(GTK_WIDGET(gui->openInspectorMenuItem), FALSE);
      gtk_widget_set_sensitive(GTK_WIDGET(gui->recentMenu), FALSE);
      suscan_gui_detach_all_inspectors(gui);
      break;
  }

  gui->state = state;

  SU_TRYCATCH(subtitle != NULL, return);

  gtk_header_bar_set_subtitle(gui->headerBar, subtitle);

  free(subtitle);
}

/************************** Async callbacks **********************************/
SUPRIVATE gboolean
suscan_async_stopped_cb(gpointer user_data)
{
  struct suscan_gui *gui = (struct suscan_gui *) user_data;
  unsigned int i;

  g_thread_join(gui->async_thread);
  gui->async_thread = NULL;

  /* Destroy all inspectors */
  for (i = 0; i < gui->inspector_count; ++i)
    if (gui->inspector_list[i] != NULL)
      gui->inspector_list[i]->inshnd = -1;

  /* Destroy analyzer object */
  suscan_analyzer_destroy(gui->analyzer);
  gui->analyzer = NULL;

  /* Consume any pending messages */
  suscan_analyzer_consume_mq(&gui->mq_out);

  if (gui->state == SUSCAN_GUI_STATE_QUITTING) {
    /*
     * Stopped was caused by a transition to QUITTING. Destroy GUI
     * and exit main loop
     */
    suscan_gui_store_recent(gui);

    suscan_gui_store_analyzer_params(gui);

    suscan_gui_destroy(gui);

    gtk_main_quit();
  } else {
    /* Update GUI with new state */
    suscan_gui_update_state(gui, SUSCAN_GUI_STATE_STOPPED);
  }

  return G_SOURCE_REMOVE;
}

SUPRIVATE gboolean
suscan_async_update_channels_cb(gpointer user_data)
{
  struct suscan_gui_msg_envelope *envelope;
  PTR_LIST(struct sigutils_channel, channel);
  SUFLOAT cpu;
  char cpu_str[10];
  unsigned int i;
  GtkTreeIter new_element;

  envelope = (struct suscan_gui_msg_envelope *) user_data;

  cpu = envelope->gui->analyzer->cpu_usage;

  snprintf(cpu_str, sizeof(cpu_str), "%.1lf%%", cpu * 100);

  gtk_label_set_text(envelope->gui->cpuLabel, cpu_str);
  gtk_level_bar_set_value(envelope->gui->cpuLevelBar, cpu);

  /* Move channel list to GUI */
  suscan_analyzer_channel_msg_take_channels(
      (struct suscan_analyzer_channel_msg *) envelope->private,
      &channel_list,
      &channel_count);
  suscan_gui_spectrum_update_channels(
      &envelope->gui->main_spectrum,
      channel_list,
      channel_count);

  if (channel_count > SUSCAN_GUI_MAX_CHANNELS)
    channel_count = SUSCAN_GUI_MAX_CHANNELS;

  /* Update channel list */
  gtk_list_store_clear(envelope->gui->channelListStore);
  for (i = 0; i < channel_count; ++i) {
    gtk_list_store_append(
        envelope->gui->channelListStore,
        &new_element);
    gtk_list_store_set(
        envelope->gui->channelListStore,
        &new_element,
        0, channel_list[i]->fc,
        1, channel_list[i]->snr,
        2, channel_list[i]->S0,
        3, channel_list[i]->N0,
        4, channel_list[i]->bw,
        -1);
  }

  suscan_gui_msg_envelope_destroy(envelope);

  return G_SOURCE_REMOVE;
}

SUPRIVATE gboolean
suscan_async_update_main_spectrum_cb(gpointer user_data)
{
  struct suscan_gui_msg_envelope *envelope;
  struct suscan_analyzer_psd_msg *msg;
  char N0_str[20];

  envelope = (struct suscan_gui_msg_envelope *) user_data;

  msg = (struct suscan_analyzer_psd_msg *) envelope->private;

  snprintf(N0_str, sizeof(N0_str), "%.1lf dBFS", SU_POWER_DB(msg->N0));

  gtk_label_set_text(envelope->gui->n0Label, N0_str);
  gtk_level_bar_set_value(
      envelope->gui->n0LevelBar,
      1e-2 * (SU_POWER_DB(msg->N0) + 100));

  suscan_gui_spectrum_update(
      &envelope->gui->main_spectrum,
      msg);

  suscan_gui_msg_envelope_destroy(envelope);

  return G_SOURCE_REMOVE;
}

SUPRIVATE gboolean
suscan_async_update_inspector_spectrum_cb(gpointer user_data)
{
  struct suscan_gui_msg_envelope *envelope;
  struct suscan_analyzer_psd_msg *msg;
  struct suscan_gui_inspector *insp = NULL;

  envelope = (struct suscan_gui_msg_envelope *) user_data;
  msg = (struct suscan_analyzer_psd_msg *) envelope->private;

  SU_TRYCATCH(
      insp = suscan_gui_get_inspector(envelope->gui, msg->inspector_id),
      goto done);

  msg->fc = 0; /* Frequency reference is wrt channel's carrier */

  suscan_gui_spectrum_update(
      &insp->spectrum,
      msg);

done:
  suscan_gui_msg_envelope_destroy(envelope);

  return G_SOURCE_REMOVE;
}

SUPRIVATE gboolean
suscan_async_parse_sample_batch_msg(gpointer user_data)
{
  struct suscan_gui_msg_envelope *envelope;
  struct suscan_analyzer_sample_batch_msg *msg;
  struct suscan_gui_inspector *insp = NULL;

  envelope = (struct suscan_gui_msg_envelope *) user_data;
  msg = (struct suscan_analyzer_sample_batch_msg *) envelope->private;

  SU_TRYCATCH(
      insp = suscan_gui_get_inspector(envelope->gui, msg->inspector_id),
      goto done);

  /* Append all these samples to the inspector GUI */
  suscan_gui_inspector_feed_w_batch(insp, msg);

done:
  suscan_gui_msg_envelope_destroy(envelope);

  return G_SOURCE_REMOVE;
}

SUPRIVATE gboolean
suscan_async_parse_inspector_msg(gpointer user_data)
{
  struct suscan_gui_msg_envelope *envelope;
  struct suscan_analyzer_inspector_msg *msg;
  struct suscan_gui_inspector *new_insp = NULL;
  struct suscan_gui_inspector *insp = NULL;
  char text[64];
  envelope = (struct suscan_gui_msg_envelope *) user_data;
  msg = (struct suscan_analyzer_inspector_msg *) envelope->private;

  /* Analyze inspector message type */
  switch (msg->kind) {
    case SUSCAN_ANALYZER_INSPECTOR_MSGKIND_OPEN:
      /* Create new inspector and append to tab */
      SU_TRYCATCH(
          new_insp = suscan_gui_inspector_new(
              &msg->channel,
              msg->handle),
          goto done);

      SU_TRYCATCH(
          suscan_gui_add_inspector(
              envelope->gui,
              new_insp),
          goto done);

      /* TODO: Set params */
      new_insp = NULL;
      break;

    case SUSCAN_ANALYZER_INSPECTOR_MSGKIND_INFO:
      SU_TRYCATCH(
          insp = suscan_gui_get_inspector(envelope->gui, msg->inspector_id),
          goto done);

      if (msg->req_id == 0) {
        /* Update from FAC */
        snprintf(text, sizeof(text), "%lg", msg->baud.fac);
        gtk_entry_set_text(insp->baudRateEntry, text);
      } else {
        /* Update from non-linear */
        snprintf(text, sizeof(text), "%lg", msg->baud.nln);
        gtk_entry_set_text(insp->baudRateEntry, text);
      }

      gtk_widget_set_sensitive(GTK_WIDGET(insp->baudRateEntry), TRUE);

      break;

    case SUSCAN_ANALYZER_INSPECTOR_MSGKIND_PARAMS:
      /* TODO: update GUI according to params */
      SU_TRYCATCH(
          insp = suscan_gui_get_inspector(envelope->gui, msg->inspector_id),
          goto done);
      SU_TRYCATCH(
          suscan_gui_inspector_update_sensitiveness(insp, &msg->params),
          goto done);
      break;

    case SUSCAN_ANALYZER_INSPECTOR_MSGKIND_CLOSE:
      SU_TRYCATCH(
          insp = suscan_gui_get_inspector(envelope->gui, msg->inspector_id),
          goto done);
      SU_TRYCATCH(
          suscan_gui_remove_inspector(envelope->gui, insp),
          goto done);

      new_insp = insp; /* To be deleted at cleanup */

      break;

    case SUSCAN_ANALYZER_INSPECTOR_MSGKIND_WRONG_HANDLE:
      suscan_error(
          envelope->gui,
          "Suscan inspector",
          "Invalid inspector handle passed");
      break;

    case SUSCAN_ANALYZER_INSPECTOR_MSGKIND_WRONG_KIND:
      suscan_error(
          envelope->gui,
          "Suscan inspector",
          "Invalid command passed to inspector");
      break;

    default:
      SU_WARNING("Ignored inspector message %d\n", msg->kind);
  }

done:
  if (new_insp != NULL)
    suscan_gui_inspector_destroy(new_insp);

  suscan_gui_msg_envelope_destroy(envelope);

  return G_SOURCE_REMOVE;
}

SUPRIVATE gpointer
suscan_gui_async_thread(gpointer data)
{
  struct suscan_gui *gui = (struct suscan_gui *) data;
  struct suscan_gui_msg_envelope *envelope;
  void *private;
  uint32_t type;

  for (;;) {
    private = suscan_analyzer_read(gui->analyzer, &type);

    switch (type) {
      case SUSCAN_WORKER_MSG_TYPE_HALT: /* Halt response */
        g_idle_add(suscan_async_stopped_cb, gui);
        /* This message is empty */
        goto done;

      case SUSCAN_ANALYZER_MESSAGE_TYPE_CHANNEL:
        if ((envelope = suscan_gui_msg_envelope_new(
            gui,
            type,
            private)) == NULL) {
          suscan_analyzer_dispose_message(type, private);
          break;
        }

        g_idle_add(suscan_async_update_channels_cb, envelope);
        break;

      case SUSCAN_ANALYZER_MESSAGE_TYPE_PSD:
        if ((envelope = suscan_gui_msg_envelope_new(
            gui,
            type,
            private)) == NULL) {
          suscan_analyzer_dispose_message(type, private);
          break;
        }

        g_idle_add(suscan_async_update_main_spectrum_cb, envelope);
        break;

      case SUSCAN_ANALYZER_MESSAGE_TYPE_INSPECTOR:
        if ((envelope = suscan_gui_msg_envelope_new(
            gui,
            type,
            private)) == NULL) {
          suscan_analyzer_dispose_message(type, private);
          break;
        }

        g_idle_add(suscan_async_parse_inspector_msg, envelope);
        break;

      case SUSCAN_ANALYZER_MESSAGE_TYPE_SAMPLES:
        if ((envelope = suscan_gui_msg_envelope_new(
            gui,
            type,
            private)) == NULL) {
          suscan_analyzer_dispose_message(type, private);
          break;
        }

        g_idle_add(suscan_async_parse_sample_batch_msg, envelope);
        break;

      case SUSCAN_ANALYZER_MESSAGE_TYPE_INSP_PSD:
        if ((envelope = suscan_gui_msg_envelope_new(
            gui,
            type,
            private)) == NULL) {
          suscan_analyzer_dispose_message(type, private);
          break;
        }

        g_idle_add(suscan_async_update_inspector_spectrum_cb, envelope);
        break;

      case SUSCAN_ANALYZER_MESSAGE_TYPE_EOS: /* End of stream */
        g_idle_add(suscan_async_stopped_cb, gui);
        suscan_analyzer_dispose_message(type, private);
        goto done;

      default:
        suscan_analyzer_dispose_message(type, private);
    }
  }

done:
  return NULL;
}

/************************** GUI Thread functions *****************************/
SUBOOL
suscan_gui_connect(struct suscan_gui *gui)
{
  unsigned int i;

  assert(gui->state == SUSCAN_GUI_STATE_STOPPED);
  assert(gui->analyzer == NULL);
  assert(gui->selected_config != NULL);

  for (i = 0; i < gui->inspector_count; ++i)
    if (gui->inspector_list[i] != NULL)
      break;

  if (i < gui->inspector_count)
    suscan_warning(
        gui,
        "Existing inspectors",
        "The opened inspector tabs will remain in idle state");

  if ((gui->analyzer = suscan_analyzer_new(
      &gui->analyzer_params,
      gui->selected_config->config,
      &gui->mq_out)) == NULL)
    return SU_FALSE;

  /* Analyzer created, create async thread */
  SU_TRYCATCH(
      gui->async_thread = g_thread_new(
          "async-task",
          suscan_gui_async_thread,
          gui),
      goto fail);

  /* Append recent. Not critical */
  (void) suscan_gui_append_recent(gui, gui->selected_config->config);

  /* Change state and succeed */
  suscan_gui_update_state(gui, SUSCAN_GUI_STATE_RUNNING);

  return SU_TRUE;

fail:
  if (gui->analyzer != NULL) {
    suscan_analyzer_destroy(gui->analyzer);
    gui->analyzer = NULL;

    suscan_analyzer_consume_mq(&gui->mq_out);
  }

  return SU_FALSE;
}

void
suscan_gui_disconnect(struct suscan_gui *gui)
{
  assert(gui->state == SUSCAN_GUI_STATE_RUNNING);
  assert(gui->analyzer != NULL);

  suscan_gui_update_state(gui, SUSCAN_GUI_STATE_STOPPING);

  suscan_analyzer_req_halt(gui->analyzer);
}

void
suscan_gui_quit(struct suscan_gui *gui)
{
  if (gui->state == SUSCAN_GUI_STATE_RUNNING) {
    /* GUI is running, ask async thread politely to quit */
    suscan_gui_update_state(gui, SUSCAN_GUI_STATE_QUITTING);

    suscan_analyzer_req_halt(gui->analyzer);
  } else if (gui->state == SUSCAN_GUI_STATE_STOPPED) {
    /* GUI already stopped, proceed to stop safely */
    suscan_gui_store_recent(gui);

    suscan_gui_store_analyzer_params(gui);

    suscan_gui_destroy(gui);

    gtk_main_quit();
  }

  /* Ignore other states */
}


