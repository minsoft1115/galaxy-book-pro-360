#include <fprint.h>

typedef struct
{
  GMainLoop *loop;
  FpDevice *device;
  GCancellable *cancel_open;
  guint cancel_timeout_id;
  guint overall_timeout_id;
  gboolean first_open_done;
  gboolean cancel_sent;
  gboolean retrying;
  gboolean passed;
  gboolean inconclusive;
  gchar *first_error;
} TestState;

static gboolean
cancel_first_open (gpointer user_data)
{
  TestState *state = user_data;

  state->cancel_timeout_id = 0;
  if (!state->first_open_done)
    {
      state->cancel_sent = TRUE;
      g_cancellable_cancel (state->cancel_open);
      g_print ("Requested cancellation of the first OPEN.\n");
    }

  return G_SOURCE_REMOVE;
}

static gboolean
overall_timeout (gpointer user_data)
{
  TestState *state = user_data;

  state->overall_timeout_id = 0;
  g_printerr ("Timed out while opening or closing the EH57E.\n");
  g_main_loop_quit (state->loop);
  return G_SOURCE_REMOVE;
}

static void
on_close_ready (GObject *source_object, GAsyncResult *result, gpointer user_data)
{
  TestState *state = user_data;
  g_autoptr(GError) error = NULL;

  if (!fp_device_close_finish (FP_DEVICE (source_object), result, &error))
    {
      g_printerr ("CLOSE failed: %s\n", error->message);
      state->passed = FALSE;
    }

  g_main_loop_quit (state->loop);
}

static void
close_device (TestState *state)
{
  fp_device_close (state->device, NULL, on_close_ready, state);
}

static void
on_open_ready (GObject *source_object, GAsyncResult *result, gpointer user_data)
{
  TestState *state = user_data;
  g_autoptr(GError) error = NULL;
  gboolean opened;

  opened = fp_device_open_finish (FP_DEVICE (source_object), result, &error);

  if (!state->retrying)
    {
      state->first_open_done = TRUE;

      if (state->cancel_timeout_id)
        {
          g_source_remove (state->cancel_timeout_id);
          state->cancel_timeout_id = 0;
        }

      if (opened)
        {
          state->inconclusive = TRUE;
          g_print ("First OPEN succeeded before an error was observed; "
                   "cancellation did not reproduce the failure.\n");
          close_device (state);
          return;
        }

      state->first_error = g_strdup (error->message);
      g_print ("First OPEN failed as intended: %s\n", state->first_error);

      state->retrying = TRUE;
      fp_device_open (state->device, NULL, on_open_ready, state);
      return;
    }

  if (!opened)
    {
      g_printerr ("Retry OPEN failed after the first OPEN error: %s\n",
                  error->message);
      g_main_loop_quit (state->loop);
      return;
    }

  g_print ("Retry OPEN succeeded on the same FpDevice object.\n");
  state->passed = TRUE;
  close_device (state);
}

int
main (void)
{
  g_autoptr(FpContext) context = fp_context_new ();
  GPtrArray *devices;
  TestState state = { 0 };
  guint cancel_after_ms = 20;

  const gchar *delay = g_getenv ("EGIS057E_OPEN_CANCEL_AFTER_MS");
  if (delay)
    cancel_after_ms = (guint) g_ascii_strtoull (delay, NULL, 10);

  state.loop = g_main_loop_new (NULL, FALSE);
  state.cancel_open = g_cancellable_new ();

  devices = fp_context_get_devices (context);
  for (guint i = 0; i < devices->len; i++)
    {
      FpDevice *device = g_ptr_array_index (devices, i);

      if (g_strcmp0 (fp_device_get_driver (device), "egis057e") == 0)
        {
          state.device = g_object_ref (device);
          break;
        }
    }

  if (!state.device)
    {
      g_printerr ("No EH57E device was found by libfprint.\n");
      return 2;
    }

  g_print ("Testing %s (%s); first OPEN cancellation delay: %u ms.\n",
           fp_device_get_name (state.device),
           fp_device_get_device_id (state.device),
           cancel_after_ms);

  state.overall_timeout_id = g_timeout_add_seconds (20, overall_timeout, &state);
  state.cancel_timeout_id = g_timeout_add (cancel_after_ms, cancel_first_open,
                                            &state);
  fp_device_open (state.device, state.cancel_open, on_open_ready, &state);
  g_main_loop_run (state.loop);

  if (state.cancel_timeout_id)
    g_source_remove (state.cancel_timeout_id);
  if (state.overall_timeout_id)
    g_source_remove (state.overall_timeout_id);

  g_clear_object (&state.device);
  g_clear_object (&state.cancel_open);
  g_clear_pointer (&state.loop, g_main_loop_unref);
  g_free (state.first_error);

  if (state.passed)
    return 0;
  if (state.inconclusive)
    return 2;
  return 1;
}
