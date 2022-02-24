# Tracker Miners

Tracker is an efficient search engine and
[triplestore](https://en.wikipedia.org/wiki/Triplestore) for desktop, embedded
and mobile.

The Tracker project is divided into two main repositories:

  * [Tracker SPARQL](https://gitlab.gnome.org/GNOME/tracker) contains the
    triplestore database, provided as the `libtracker-sparql` library
    and implemented using [SQLite](http://sqlite.org/). This repo also contains
    the database ontologies and the commandline user interface (`tracker3`).

  * [Tracker Miners](https://gitlab.gnome.org/GNOME/tracker-miners) contains
    the indexer daemon (*tracker-miner-fs-3*) and tools to extract metadata
    from many different filetypes.

More information on Tracker can be found at:

  * <https://gnome.pages.gitlab.gnome.org/tracker/>
  * <https://wiki.gnome.org/Projects/Tracker>

Source code and issue tracking:

  * <https://gitlab.gnome.org/GNOME/tracker>
  * <https://gitlab.gnome.org/GNOME/tracker-miners>

All discussion related to Tracker happens on:

  * <https://discourse.gnome.org/tag/tracker>

IRC channel #tracker on:

  * [irc.gimp.net](irc://irc.gimp.net)

Related projects:

  * [GNOME Online Miners](https://gitlab.gnome.org/GNOME/gnome-online-miners/)
    extends Tracker to allow searching and indexing some kinds of online
    content.

For more information about developing Tracker, look at the
[README.md file in Tracker core](https://gitlab.gnome.org/GNOME/tracker/tree/master/README.md).
