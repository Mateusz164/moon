#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "uri.h"

class Assert {
public:
  static void AreEqual (const char *expected, const char *value, const char *msg = NULL) {
    if (!value || strcmp (expected, value)) {
      char *fail = g_strdup_printf ("expected `%s', got `%s'", expected, value);
      failure (fail, msg);
    }
  }

  static void AreEqual (int expected, int value, const char *msg = NULL) {
    if (expected != value) {
      char *fail = g_strdup_printf ("expected `%d', got `%d'", expected, value);
      failure (fail, msg);
    }
  }

  static void IsNull (gpointer p, const char *msg = NULL) {
    if (!p) return;
    failure ("pointer was not null", msg);
  }

  static void IsTrue (bool b, const char *msg = NULL) {
    if (b) return;
    failure ("expected true", msg);
  }

  static void IsFalse (bool b, const char *msg = NULL) {
    if (!b) return;
    failure ("expected true", msg);
  }

private:
  static void failure (const char *msg, const char *user_msg) {
    if (user_msg)
      fprintf (stderr, "FAILURE:  %s: %s\n", msg, user_msg);
    else
      fprintf (stderr, msg);
  }
};


int
main()
{
	Uri *uri = new Uri();

	Assert::IsTrue (uri->Parse ("http://moonlightmedia:81/source/robotica.wmv"), "1 ::Parse");
	Assert::AreEqual ("http", uri->protocol, "1 protocol");
	Assert::AreEqual ("moonlightmedia", uri->host, "1 host");
	Assert::AreEqual (81, uri->port, "1 port");
	Assert::AreEqual ("/source/robotica.wmv",uri->path, "1 path");

	Assert::IsTrue (uri->Parse ("ribbon/images/DialogBoxLauncher.png"), "2 ::Parse");
	Assert::IsNull (uri->protocol, "2 protocol");
	Assert::IsNull (uri->host, "2 host");
	Assert::AreEqual (-1, uri->port, "2 port");
	Assert::AreEqual ("ribbon/images/DialogBoxLauncher.png",uri->path, "2 path");
}
