/* PGO training helper (LD_PRELOAD): quits the application cleanly after
 * PGO_SECONDS so the instrumented binary writes its profile at exit. */
#include <gio/gio.h>
#include <stdlib.h>
static gboolean quit (gpointer) {
  GApplication *app = g_application_get_default ();
  if (app) g_application_quit (app); else exit (0);
  return G_SOURCE_REMOVE;
}
__attribute__((constructor)) static void init (void) {
  g_timeout_add_seconds (atoi (getenv ("PGO_SECONDS") ?: "20"), quit, NULL);
}
