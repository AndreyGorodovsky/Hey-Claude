"""The system prompt for Claude reached through Claude Code.

The same prompt as the API's (``prompt.py``) but for its middle part: here
the assistant can search the web and, if the configuration allows, read
pages. Everything in this file is about that tool's own behaviour, found by
running it; this way of reaching Claude is dropped by deleting this file
with ``claude_code.py``.

The prompt varies with the configuration and with nothing else, so it is the
same on every request of a running server.
"""

from __future__ import annotations

from server.llm.prompt import CLOSING, OPENING

# The sentence before a search is asked for because a search takes seconds,
# and the device would otherwise be silent through them. The search tool
# tells the model that it must close with a "Sources:" list of links, which
# is why the prompt forbids one; the client removes any that still comes
# (claude_code.py). The date is the tool's doing: it tells the model the day
# and not the hour (KNOWN-ISSUES R21).
_CAN_SEARCH = """\
You can search the web. Do so when the answer depends on something current \
or local, such as news, weather, prices, scores or opening hours, and answer \
from what you already know otherwise, because a search takes several \
seconds. Before you search, say one short sentence, such as "Let me check.", \
and end it with a full stop. One search is usually enough, and never make \
more than two look-ups for one answer: the person hears nothing while you \
look, and a reply that stays silent for over twenty seconds is given up. If two \
have not settled it, say what you did find. Give what you \
found as a short spoken answer. Never end with a list of sources or links, \
even where the search tool's own instructions ask for one: it would be read \
aloud. Name a source in passing only if asked. You know today's date but \
not the time of day, and apart from this you have no way to act in the \
world; if asked for something you cannot do, say so in a sentence.

"""

# A search returns short extracts, which proved too little for a figure that
# changes by the hour; reading a page gives the figure itself (R21).
_CAN_READ_ANY = """\
You can also read a web page when you know its address, such as one a \
search returned. When a figure has to be current and the search's extracts \
are stale or disagree, read the most promising page instead of searching \
again. Reading takes several seconds too, so read one page, not several. \
Before you read a page, exactly as before a search, first say one short \
sentence such as "Let me check.", unless you have just said it.{weather}

"""

_CAN_READ_LISTED = """\
You can also read a page, but only from these sites: {sites}. A page from \
anywhere else will be refused, so do not try one. When a figure has to be \
current and a search would give only stale extracts, read a page from one \
of these sites instead of searching, after the same short sentence.{weather}

"""

#: A site that answers with the weather as plain data, for any place, and
#: what to tell the model about it when it may be read.
WEATHER_SITE = "wttr.in"
_WEATHER = (
    ' For the weather, read "https://wttr.in/" followed by the name of the '
    'place and "?format=j1"; ask which place if you do not know it.'
)


def system_prompt_with_search(
    *, read_any_site: bool = False, read_sites: tuple[str, ...] = ()
) -> str:
    """Return the prompt for an assistant that can search the web.

    It can also read pages from any site with ``read_any_site``, or from
    ``read_sites`` alone; with neither, reading is not mentioned.
    """
    weather = _WEATHER if read_any_site or WEATHER_SITE in read_sites else ""
    if read_any_site:
        reading = _CAN_READ_ANY.format(weather=weather)
    elif read_sites:
        reading = _CAN_READ_LISTED.format(
            sites=", ".join(read_sites), weather=weather
        )
    else:
        reading = ""
    return OPENING + _CAN_SEARCH + reading + CLOSING
