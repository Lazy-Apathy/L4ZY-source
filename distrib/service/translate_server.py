#!/usr/bin/env python3
"""
Service de traduction local pour Cube 2: Sauerbraten.

Le jeu envoie une ligne de chat, ce service la traduit et la renvoie.
Bibliotheque standard uniquement : rien a installer avec pip.

Lancement :   python3 translate_server.py
Test manuel : curl -X POST --data "hijo de puta" "http://127.0.0.1:8747/translate?lang=fr"

Prerequis : llama-server (demarre tout seul si runtime.cfg pointe vers un .gguf).
"""

import argparse
import http.server
import json
import os
import re
import socketserver
import sys
import threading
import time
import urllib.error
import urllib.request
from collections import OrderedDict
from urllib.parse import parse_qs, urlparse

import sauerrt_paths as paths
import updater
import llama_runtime
import menu_plan
import model_manager

# ---------------------------------------------------------------------------
# CONFIGURATION - c'est la seule partie que tu as besoin de modifier
#
# Les quatre premieres valeurs peuvent aussi etre passees en ligne de commande
# (--model, --port, --quiet, --idle-exit) : c'est ce que font les lanceurs, pour
# qu'une machine moins puissante n'ait pas besoin d'un fichier different.
# ---------------------------------------------------------------------------

LISTEN_PORT = 8747

LLM_URL = "http://127.0.0.1:8080/v1/chat/completions"
MODEL = "qwen3"

IDLE_EXIT_MIN = 0      # 0 = le service reste en vie ; sinon il s'arrete tout
                       # seul apres N minutes sans aucune demande

# Les modeles "raisonneurs" (Qwen3, DeepSeek-R1...) reflechissent avant de
# repondre. Sur une ligne de chat de cinq mots c'est plusieurs secondes perdues
# pour rien : c'est LA cause la plus frequente d'une traduction lente.
DISABLE_THINKING = True

TEMPERATURE = 0.1      # bas = moins d'inventions ; 0.2 laissait trop derivé
MAX_TOKENS = 80        # une ligne de chat est courte, inutile de laisser filer
NUM_CTX = 4096         # Qwen3 defaut ~32k : KV cache enorme, le jeu et le
                       # modele se marchent sur la VRAM, apercu de plusieurs s
                       # llama-server : meme -c, et -np 1 (pas 4 slots)
# 3 lignes, pas 6 : assez pour "it" / "ok" / "me too", trop peu pour que le
# modele confonde l'historique avec la phrase a traduire. L'historique va
# dans le prompt SYSTEME, jamais dans le message user (sinon il repond "=").
CONTEXT_LINES = 3
HISTORY_IDLE = 120     # secondes sans chat -> on oublie le sujet precedent
CACHE_SIZE = 2000
# Paires /tfix et menu examples. Deux sortes :
#   exact = cette phrase seulement (pas de few-shot, l'IA ne ralentit pas)
#   ex    = few-shot : autant que tu veux, plus il y en a plus l'IA travaille
EXAMPLES_FILE = ""  # fixe dans main() : dossier personnel, jamais dans les fichiers distribues

VERBOSE = True         # affiche chaque traduction et sa latence

# Termes laisses tels quels : ce sont du jargon, pas des mots.
GLOSSARY = """
gg ggs wp nt gl hf glhf gj ns ez rq afk brb gtg g2g bb ty np yw lol lmao xd
rip noob nub camper camping spawn spawnkill frag ragequit tk ff ctf ictf
ectf insta instagib dm ffa tdm duel mix cw mid base flag cap def inc push
rush clutch whiff hs headshot ping lag fps admin master spec votekick rtv
respawn ammo hp sauer sauerbraten aimbot wallhack wh triggerbot esp radar
pban hud vc demo ogro teamchat tbh ngl low cheater dogwater lube
effic hfhf wtf comeback sub map
""".split()

# ---------------------------------------------------------------------------

SYSTEM_PROMPT = """\
You translate live chat from the multiplayer FPS "Cube 2: Sauerbraten", game \
mode instagib CTF. Messages are short, informal, misspelled and often rude. \
Translate them into {lang}.

OUTPUT RULES (these matter more than anything else):
- Output ONLY the translated line. No quotes, no notes, no explanation, no \
alternatives, no preamble, no thinking tags, no /no_think.
- Output at most one line.
- Never copy the input if it is not already in {lang}. English, Spanish, \
German, Italian, Portuguese, French, Turkish, Polish, Russian, etc. MUST \
be translated when they are not {lang}. If the source is full of typos \
(ar=are, eu=you, u=you, rn=right now), decode it then translate. Never \
echo the misspelled source. Never write a note such as "this is Turkish" \
or "no need to translate": output the translation itself.
- If the message is ALREADY in {lang}, output exactly: =
- If the message is pure jargon (gg, wp, ns, mid, flag) with no other words, \
output exactly: =
- Do not use = for a sentence that is clearly in another language. \
French is not English. English is not French.

STYLE RULES:
- Preserve the register. This is trash talk between players: do NOT soften it, \
do NOT make it polite. Decode typos in the source; the output must be readable \
{lang}, not a copy of the misspelled input.
- Stay as short as the original. A two-word callout stays a two-word callout.
- Leave player names, clan tags, map names, mode names, game titles \
(Fear and Hunger) and numbers exactly as written. Do not turn a nickname \
into a common noun (Apathy is not apathie, Fear is not peur, COW is not \
vache, LAZY is the clan, tool, buttman, Scuba, nasty/nasti, woals, \
watson, fiko, tribu, gavin, kotzi are players). \
o/ plus a name is a wave, not a translation of the name.
- Never invent. If the source does not mention the flag, a death, or a \
map, do not add them. Do not reverse less / never / not.
- Leave slash-commands unchanged. If the whole line is a command, output =.
- Never add emojis, never add /no_think, never explain an acronym.
- ESC is the Escape key, not "flee". HELL is emphasis, not "demon". \
nw = no worries. gn = good night. tbh = to be honest. ngl = not gonna lie. \
got it = I understand / ok, NOT the flag unless the line is a flag callout. \
When translating into French, keep English role-words (cheater, noob) \
as-is and put le in front (le cheater). Use la only if the source \
clearly marks a woman (she, her).
- Leave this jargon untranslated: {glossary}
"""

# L'historique va APRES les few-shot, pas dans le systeme : llama-server peut
# cacher le prefixe (consignes + exemples), identique d'une ligne a l'autre.
# L'avoir dans le systeme forçait a tout recalculer a chaque message.
CONTEXT_ADDENDUM = """
BACKGROUND CHAT (already happened; NOT the next line):
{chat}

Use background only for pronouns and short replies (it, that, him, them, ok, \
me too, coming, same). The next user message is the only line to translate \
into {lang}. Do not output = because a background line is already in {lang}. \
Do not borrow the flag, a kill, or another language from the background.
"""

# Collé pour un message que le joueur va envoyer : on vise une phrase native, pas
# un calque, parce que les autres n'ont pas l'original sous les yeux.
OUT_ADDENDUM = """
This is the player's own outgoing chat, about to be sent. Write a native {lang} \
line a Sauerbraten player would actually type. No calque, no extra words, no \
explanation. Keep it as short as the original. Never output = unless the line \
is already native {lang}. French (salut, ca va, je, tu) is not English.
"""

# Case menu "I am a woman" (cvar translatefemme -> &genre=f). Ne pas accrocher
# ca a un nom de fichier GGUF : c'est du prompt, comme le reste.
FEMME_OUT = """
The player who wrote this line is a woman. First-person and self-reference \
MUST use feminine agreement in {lang} wherever that language inflects \
(French: je suis prete/tombee/contente/seule/morte; Spanish: estoy \
lista/cansada/sola; Italian: sono pronta/stanca/sola; Portuguese: to \
pronta/cansada/sozinha; German: feminine forms when they inflect, e.g. \
Deutsche not Deutscher). Do not call the speaker dude, man, bro, mate, \
guy, mec. Second-person about OTHER players stays unmarked (listo, pronto, \
pret) unless the source marks a woman (she, her, elle).
"""

FEMME_IN = """
The player reading this chat is a woman. When the line addresses the reader \
in the 2nd person (you, you're, u, dude, man, mate, bro), use feminine \
agreement in {lang} (you're bad -> t'es nulle; you're ready -> t'es prete; \
hey man -> salut, not salut mec). This overrides the le/la default for \
2nd person. Do not change 1st-person or 3rd-person about OTHER players \
unless the source marks a woman (he's low stays il est low; she/her -> \
feminine). Role-words (cheater, noob): une/la only when addressing the \
reader or when the source marks a woman.
"""

# Ces exemples font plus pour la qualite que n'importe quel reglage : ils
# montrent au modele le registre attendu et l'usage du sentinel "=".
#
# Ils sont indexes par LANGUE CIBLE, jamais par sens de traduction. Un jeu
# d'exemples enseigne une seule chose : "quelle que soit la langue d'entree,
# tu reponds dans CETTE langue". C'est vrai aussi bien pour un message recu que
# pour un /tsay, donc la meme liste sert aux deux.
#
# Les langues d'entree sont volontairement melangees dans chaque liste : c'est
# ce qui apprend au modele a ne pas recopier la langue de depart.
#
# Les paires ci-dessous viennent surtout des vrais chats JKEU (aout 2026).
# On reste volontairement court : chaque exemple est renvoye au modele a
# CHAQUE ligne, et un prompt trop long casse le cache (gris d'abord, vert
# plusieurs secondes plus tard).
FEWSHOT = {
    "fr": [
        ("hijo de puta",                     "fils de pute"),
        ("geh mitte, ich hol die flagge",    "va mid, je prends le flag"),
        ("eles estao vindo por tras",        "ils arrivent par derriere"),
        ("ti stai divertendo a camperare?",  "ca t'amuse de camper ?"),
        ("you are cheating for sure",        "tu triches c'est sur"),
        ("Hey mate, how are you",            "salut ca va"),
        ("SHUT OFF THE GODDAMN AIMBOT!!!!",  "ARRETE LE PUTAIN D AIMBOT !!!!"),
        ("AR EU SERIOURS RIGHT NOW",         "t'es serieux la"),
        ("GET GOOD, EVEYONE!!!",             "ameliorez vous tous !!!"),
        ("fuck",                             "putain"),
        ("fuck you",                         "va te faire foutre"),
        ("coming",                           "j'arrive"),
        ("got it",                           "ok je l'ai"),
        ("he's low",                         "il est low"),
        ("whats up",                         "quoi de neuf"),
        ("only yes",                         "seulement oui"),
        ("nw",                               "y a pas de souci"),
        ("gn",                               "bonne nuit"),
        ("no 10",                            "pas 10"),
        ("yeah not a chance",                "ouais aucun chance"),
        ("how the HELL is it more important?", "comment ca peut etre plus important putain ?"),
        ("esc and press toggle spectator",   "echap puis toggle spectator"),
        ("whats the command to zoom out the minimap more?", "c'est quoi la commande pour dezoomer plus la minimap ?"),
        ("apathy wasn't enough :)",          "apathy suffisait pas :)"),
        ("cow is busy atm",                  "COW est occupe"),
        ("lazy clan play with us",           "LAZY jouez avec nous"),
        ("LAZY are lazy",                    "LAZY sont des feignasses"),
        ("poor map taste",                   "mauvais gout en map"),
        ("watch out i shoot back",           "attention je tire en retour"),
        ("sprich deutsch du hurensohn",      "parle allemand fils de pute"),
        ("ok cheater is here again i go byw", "ok le cheater est de retour je pars bye"),
        ("the cheater is back, she's using aimbot", "la cheater est de retour, elle utilise un aimbot"),
        ("how lazy",                         "trop lazy"),
        ("bb thx for playing",               "bb merci d'avoir joue"),
        ("eays win",                         "easy win"),
        ("take your sweet ass time",         "prends ton temps"),
        ("i just did poor in insta",         "j'ai mal joue en insta"),
        ("i'll be his effic sub",            "je serai son sub effic"),
        ("they are bored to win or?",        "ils en ont marre de gagner ou quoi ?"),
        ("is that a fear and hunger cit?",   "c'est une ref fear and hunger ?"),
        ("u fais durer le plaisir",          "="),
        ("pas la peine de continuer",        "="),
        ("fini",                             "="),
        ("gg wp",                            "="),
        ("b mid",                            "="),
        ("/bind key [suicide]",              "="),
    ],
    # Les paires "carte -> map" et "drapeau -> flag" sont la expres : hors
    # contexte un modele rend "carte" par "card", et l'exemple verrouille le
    # sens. Chaque langue a ses propres faux amis, a traiter de la meme facon.
    "en": [
        ("cette carte est vraiment bien",    "this map is really good"),
        ("il est ou le drapeau",             "where is the flag"),
        ("va mid, je prends le flag",        "go mid, ill take the flag"),
        ("hijo de puta",                     "son of a bitch"),
        ("arrete de camper la reapparition", "stop camping the spawn"),
        ("attention il arrive par derriere", "careful hes coming from behind"),
        ("je suis pas en train de tricher",  "im not cheating"),
        ("salut",                            "hey"),
        ("salut mec",                        "hey"),
        ("salut comment ca va",              "hey how are you"),
        ("sache que tu es vraiment incroyable", "you should know you're really amazing"),
        ("tu es allemand butty ?",           "are you german butty?"),
        ("j'arrive",                         "coming"),
        ("ok je l'ai",                       "got it"),
        ("je t'aime bien oui",               "i like you yeah"),
        ("oui",                              "yes"),
        ("ouais",                            "yeah"),
        ("non",                              "no"),
        ("gg wp",                            "="),
        ("nice shot dude",                   "="),
        ("b mid",                            "="),
    ],
    "de": [
        ("tu es allemand butty ?",           "bist du deutsch butty?"),
        ("salut mec comment ça va aujourd'hui ?", "hi, wie gehts dir heute?"),
        ("va mid, je prends le flag",        "geh mid, ich hol die flag"),
        ("fils de pute",                     "du hurensohn"),
        ("Et c'est instantané",              "und es ist sofort"),
        ("sache que tu es vraiment incroyable", "du solltest wissen, du bist wirklich krass"),
        ("j'arrive",                         "ich komm"),
        ("ok je l'ai",                       "hab sie"),
        ("oui",                              "ja"),
        ("non",                              "nein"),
        ("coming",                           "ich komm"),
        ("got it",                           "hab sie"),
        ("gg wp",                            "="),
        ("b mid",                            "="),
        ("servus",                           "="),
        ("geh mitte, ich hol die flagge",    "="),
    ],
    "es": [
        ("évidemment que je suis rapide",    "claro que voy rapido"),
        ("va mid, je prends le flag",        "ve mid, yo cojo la flag"),
        ("fils de pute",                     "hijo de puta"),
        ("j'arrive",                         "voy"),
        ("ok je l'ai",                       "la tengo"),
        ("oui",                              "si"),
        ("non",                              "no"),
        ("salut",                            "hola"),
        ("coming",                           "voy"),
        ("gg wp",                            "="),
        ("b mid",                            "="),
        ("hijo de puta",                     "="),
    ],
    "it": [
        ("va mid, je prends le flag",        "vai mid, prendo la flag"),
        ("fils de pute",                     "figlio di puttana"),
        ("j'arrive",                         "arrivo"),
        ("ok je l'ai",                       "ce l'ho"),
        ("oui",                              "si"),
        ("non",                             "no"),
        ("salut",                            "ciao"),
        ("coming",                           "arrivo"),
        ("gg wp",                            "="),
        ("b mid",                            "="),
        ("ti stai divertendo a camperare?",  "="),
    ],
    "pt": [
        ("va mid, je prends le flag",        "vai mid, eu pego a flag"),
        ("fils de pute",                     "filho da puta"),
        ("j'arrive",                         "to indo"),
        ("ok je l'ai",                       "peguei"),
        ("oui",                              "sim"),
        ("non",                              "nao"),
        ("salut",                            "e ai"),
        ("coming",                           "to indo"),
        ("gg wp",                            "="),
        ("b mid",                            "="),
        ("eles estao vindo por tras",        "="),
    ],
}

# Exemples extra, seulement si genre=f. Separes IN / OUT : un "je suis pret
# -> estoy lista" est juste pour ce qu'elle envoie, pas pour un autre joueur.
FEWSHOT_FEMME_OUT = {
    "en": [
        ("je suis pret",                     "im ready"),
        ("je suis prete",                    "im ready"),
        ("je suis tombe",                    "i fell"),
        ("je suis seule",                    "im alone"),
        ("je suis morte",                    "im dead"),
    ],
    "fr": [
        ("im ready",                         "je suis prete"),
        ("i fell",                           "je suis tombee"),
        ("im alone",                         "je suis seule"),
        ("im dead",                          "je suis morte"),
        ("im tired",                         "je suis fatiguee"),
    ],
    "de": [
        ("je suis pret",                     "ich bin bereit"),
        ("je suis allemande",                "ich bin deutsche"),
    ],
    "es": [
        ("je suis pret",                     "estoy lista"),
        ("je suis prete",                    "estoy lista"),
        ("je suis fatiguee",                 "estoy cansada"),
        ("je suis seule",                    "estoy sola"),
        ("je suis morte",                    "estoy muerta"),
    ],
    "it": [
        ("je suis pret",                     "sono pronta"),
        ("je suis prete",                    "sono pronta"),
        ("je suis fatiguee",                 "sono stanca"),
        ("je suis seule",                    "sono sola"),
        ("je suis morte",                    "sono morta"),
    ],
    "pt": [
        ("je suis pret",                     "to pronta"),
        ("je suis prete",                    "to pronta"),
        ("je suis fatiguee",                 "to cansada"),
        ("je suis seule",                    "to sozinha"),
        ("je suis morte",                    "to morta"),
    ],
}

FEWSHOT_FEMME_IN = {
    "fr": [
        ("you're bad",                       "t'es nulle"),
        ("you suck",                         "t'es nulle"),
        ("you're ready",                     "t'es prete"),
        ("you ready?",                       "t'es prete ?"),
        ("hey man",                          "salut"),
        ("hey dude",                         "salut"),
    ],
    "en": [
        ("t'es nulle",                       "you're bad"),
        ("t'es prete",                       "you ready"),
    ],
    "es": [
        ("you're ready",                     "estas lista"),
        ("you're bad",                       "eres mala"),
        ("hey man",                          "hola"),
    ],
    "it": [
        ("you're ready",                     "sei pronta"),
        ("you're bad",                       "sei scarsa"),
        ("hey man",                          "ciao"),
    ],
    "pt": [
        ("you're ready",                     "c ta pronta"),
        ("you're bad",                       "c e ruim"),
        ("hey man",                          "e ai"),
    ],
    "de": [
        ("you're ready",                     "bist du bereit"),
        ("hey man",                          "hi"),
    ],
}

SENTINEL = "="
THINK_RE = re.compile(r"<think>.*?</think>", re.DOTALL | re.IGNORECASE)

def llama_ctx():
    """Taille du contexte du modele local (runtime.cfg ctx), 4096 par defaut."""
    try:
        cfg, _cfg_dir = llama_runtime.load_config(str(model_manager.ensure_runtime_cfg()))
        return int(cfg.get("ctx") or 4096)
    except Exception:
        return 4096


# Assistant des reglages : plan des menus (data/assistant-menus.txt, genere a
# chaque version depuis menus.cfg). Le resume des pages remplace la liste ecrite
# a la main du prompt ; pour chaque question, on ajoute seulement la position
# exacte des reglages trouves et, si la question porte sur la disposition, la
# section autour. Sans plan (ancienne installation) : le prompt reste tel quel.
MENU_OVERVIEW_RE = re.compile(r"<menu-overview>\n?(.*?)</menu-overview>\n?", re.DOTALL)
ASSIST_ANSWER_TOKENS = 400


def assistant_menu_plan():
    try:
        return menu_plan.load(paths.ROOT / "data" / "assistant-menus.txt")
    except Exception as exc:  # un plan illisible ne doit jamais casser l'assistant
        print(f"  [assistant] plan des menus illisible : {exc}", file=sys.stderr)
        return None


def assistant_system(prompt, catalog, question, history, plan, ctx=None):
    """Prompt systeme de l'assistant : prompt (resume des menus a jour),
    catalogue (chemins du plan), puis positions / disposition des menus dans
    la place qui reste dans le contexte du modele (~3 caracteres par jeton)."""
    if plan is not None:
        ov = plan.overview()
        if ov:
            prompt = MENU_OVERVIEW_RE.sub(lambda m: ov + "\n", prompt)
        catalog = plan.rewrite_catalog(catalog)
    prompt = prompt.replace("<menu-overview>\n", "").replace("</menu-overview>\n", "")

    def build(cat):
        return (prompt.strip() + "\n\nSettings and commands for this question (name = current value (limits) : label [menu]):\n"
                + (cat.strip() or "(none)"))
    system = build(catalog)
    if plan is None:
        return system
    extra = plan.context(question, catalog, budget=1800)
    if not extra:
        return system
    # place qui reste (~3,6 caracteres par jeton mesures avec Qwen3 sur ce prompt) ;
    # si elle manque, les derniers reglages du catalogue (les moins probables,
    # le jeu les classe du meilleur au moins bon) laissent la place au plan
    ctx = ctx or llama_ctx()
    room = int((ctx - ASSIST_ANSWER_TOKENS - 120) * 3.6) - sum(len(h) for h in history) - len(question)
    lines = catalog.strip().splitlines()
    want = min(len(extra), 1800)
    while len(system) + 2 + want > room:
        settings = [k for k, l in enumerate(lines) if re.match(r"^\w+ = ", l)]
        if len(settings) <= 6:
            break
        del lines[settings[-1]]
        system = build("\n".join(lines))
    budget = room - len(system) - 2
    if budget < 250:
        return system
    extra = plan.context(question, "\n".join(lines), budget=min(budget, 1800))
    return system + ("\n\n" + extra if extra else "")


# Assistant des reglages : de quoi deviner la langue de la question.
ASSIST_FR = set("""je tu il elle on nous vous mon ma mes ton ta tes le la les un une des du de et ou est sont
comment pourquoi quoi quel quelle peux veux voudrais mets met change enleve enlève baisse monte augmente trop pas
plus moins avec sans dans sur pour oui non merci stp svp ça ca c'est où son viseur souris touche""".split())
ASSIST_EN = set("""i you my your the a an and or is are how why what which where can could would want please
put set change turn remove lower raise too not more less with without in on for yes no thanks it this that
crosshair mouse key sound volume bigger smaller louder quieter""".split())
# Qwen3 echo parfois son interrupteur de raisonnement en fin de ligne.
# /non (colle au slash) = /no_think dont "no" a ete traduit ; "oui / non"
# avec un espace apres le slash, lui, est un vrai "yes / no" : on le garde.
THINK_TAG_RE = re.compile(r"</?think>", re.IGNORECASE)
THINK_SWITCH_RE = re.compile(
    r"\s*/+\s*(?:no_think|no-think|no think)\s*$", re.IGNORECASE
)
THINK_SWITCH_FR_RE = re.compile(r"\s*/non(?:_think)?\s*$", re.IGNORECASE)
# Qwen commente parfois ("cette ligne est en turc") au lieu de traduire.
META_NOTE_RE = re.compile(
    r"(pas besoin de traduire|cette ligne est en|"
    r"no need to translate|already in (?:french|english|turkish)|"
    r"cannot translate|is in turkish)",
    re.IGNORECASE,
)

# Un mot d'affirmation / negation : trop court pour confier ca au modele
# (il inverse oui/non, ou colle /no_think). On garde la ponctuation autour.
YESNO = {
    "fr": {
        "yes": "oui", "yeah": "oui", "ye": "oui", "yea": "oui",
        "yep": "oui", "yup": "oui", "evet": "oui",
        "no": "non", "nope": "non", "nah": "non",
        "noo": "non", "nooo": "non", "noooo": "non",
        "hayir": "non", "hayır": "non",
    },
    "en": {
        "oui": "yes", "ouais": "yeah", "non": "no", "nan": "no",
    },
    "de": {
        "oui": "ja", "ouais": "ja", "non": "nein", "yes": "ja", "no": "nein",
    },
    "es": {
        "oui": "si", "ouais": "si", "non": "no", "yes": "si", "no": "no",
    },
    "it": {
        "oui": "si", "ouais": "si", "non": "no", "yes": "si", "no": "no",
    },
    "pt": {
        "oui": "sim", "ouais": "sim", "non": "nao", "yes": "sim", "no": "nao",
    },
}
FILLER = {"ok", "k", "kk", "okay"}
INTERJECTIONS = {
    "oh", "uh", "uhm", "um", "ah", "eh", "huh", "euh", "hm", "hmm",
    "uhh", "ahh", "ohh", "erm",
}
SLANG = {
    "fr": {
        "idk": "je sais pas",
        "rdy": "pret",
        "thx": "merci",
        "sry": "desole",
        "sr": "desole",
        "usry": "desole",
        "idm": "ca m'est egal",
        "ffs": "putain",
        "doin": "en train",
        "doing": "en train",
    },
}
# 1re personne seulement, donc uniquement sur ce qu'elle ENVOIE.
# Un autre joueur qui dit sry n'est pas "desolee" parce que la joueuse a coché « I am a woman ».
SLANG_FEMME = {
    "fr": {
        "rdy": "prete",
        "sry": "desolee",
        "sr": "desolee",
    },
}
# Match exact (et variantes ci-dessous dans phrase()) : 0 ms, pas de GPU.
# On met ici les lignes fausses du log, pas des dizaines de few-shot.
PHRASES = {
    "fr": {
        "i fell": "je suis tombe",
        "i fell into the void": "je suis tombe dans le vide",
        "oh boy": "oh putain",
        "y0 star": "salut star",
        "yo star": "salut star",
        "eays win": "easy win",
        "easy win": "easy win",
        "g jerb": "gj",
        "lets insta": "on fait insta",
        "let's insta": "on fait insta",
        "this is fine": "ca va",
        "lags like hell": "ca lague a mort",
        "lag like hell": "ca lague a mort",
        "lagging like hell": "ca lague a mort",
        "we got no chance": "on a aucune chance",
        "we have no chance": "on a aucune chance",
        "you got me": "t'as eu",
        "u got me": "t'as eu",
        "nediyor bu": "il dit quoi",
        "nedir bu": "c'est quoi ca",
        "neder bu": "c'est quoi ca",
        "guzel": "cool",
        "güzel": "cool",
        "tk for me": "tk pour moi",
        "i cant breath": "j'arrive plus a respirer",
        "i can't breath": "j'arrive plus a respirer",
        "i cant breathe": "j'arrive plus a respirer",
        "i can't breathe": "j'arrive plus a respirer",
        "put me in coach": "mets-moi en jeu coach",
        "comback fiko": "comeback fiko",
        "he was wanking": "il se branlait",
        "dick fromage": "bite fromage",
        "kommt mir spanisch vor": "ca me semble louche",
        "effic turbine": "=",
        "despacito": "=",
        "selam kardesim": "salut frere",
        "selam kardeşim": "salut frere",
        "same": "pareil",
        "true": "c'est vrai",
        "oops": "oups",
        "back": "je suis la",
        "go?": "on y va ?",
        "go ?": "on y va ?",
        "we ball": "c'est parti",
        "jesus christ": "putain",
        "ffs take": "putain prends",
        "re?": "=",
        "re": "=",
        "in": "=",
        "map?": "=",
        "map ?": "=",
    },
}
PHRASES_FEMME = {
    "fr": {
        "i fell": "je suis tombee",
        "i fell into the void": "je suis tombee dans le vide",
    },
}

LEARNED_HEADER = """\
# Paires ajoutees en jeu : menu traduction → onglet examples, ou /tfix.
# Deux sortes (radios du menu, aussi utilisees par /tfix) :
#   exact = traduction exacte : cette phrase seulement, l'IA ne ralentit pas
#   ex    = few-shot / exemple : autant que tu veux ; plus il y en a,
#           plus l'IA travaille (traductions plus lentes). L'avertissement suffit.
# Une ligne :
#   fr in exact | phrase originale | traduction
#   fr in ex | phrase originale | traduction
#   en out ex | phrase originale | traduction
#   fr in exact f | ... | ...     (seulement si I am a woman est coche)
# in  = chat recu
# out = ce que tu envoies (/tsay)
# Une traduction egale a = signifie : ne pas traduire (laisser tel quel).
# Une ancienne ligne sans exact/ex est traitee comme un exemple (ex).
#
# Modeles vides pour les autres langues (ne pas copier le FR) :
#   exemples-modele-pt.txt, exemples-modele-de.txt, etc.
#
# Exemple (retire le # pour l'activer) :
# fr in exact | hey dude | salut
"""


def parse_kind(tokens):
    """exact = lookup only ; ex = few-shot. Legacy (rien) = ex."""
    kind = "ex"
    for tok in tokens[2:]:
        t = (tok or "").strip().lower()
        if t in ("exact", "x"):
            kind = "exact"
        elif t in ("ex", "example", "shot", "few"):
            kind = "ex"
    return kind


def parse_learned_line(line):
    """Une ligne du fichier exemples, ou None si commentaire / vide / illisible."""
    line = (line or "").strip()
    if line.startswith("\ufeff"):
        line = line.lstrip("\ufeff").strip()
    if not line or line.startswith("#"):
        return None
    parts = line.split(" | ", 2)
    if len(parts) < 3:
        parts = [p.strip() for p in line.split("|")]
        if len(parts) < 3:
            return None
        src = parts[1]
        dst = "|".join(parts[2:]).strip()
        header = parts[0]
    else:
        header, src, dst = parts[0].strip(), parts[1].strip(), parts[2].strip()
    tokens = header.split()
    if len(tokens) < 2 or not src or not dst:
        return None
    lang = langcode(tokens[0])
    direction = tokens[1].strip().lower()
    if direction not in ("in", "out"):
        return None
    femme = any(is_femme(tok) for tok in tokens[2:])
    kind = parse_kind(tokens)
    src = src.replace("\r", " ").replace("\n", " ").strip()
    dst = dst.replace("\r", " ").replace("\n", " ").strip()
    if not src or not dst:
        return None
    return {
        "lang": lang,
        "direction": direction,
        "femme": femme,
        "kind": kind,
        "src": src,
        "dst": dst,
    }


def format_learned_line(item):
    kind = item.get("kind") or "ex"
    if kind != "exact":
        kind = "ex"
    head = f"{item['lang']} {item['direction']} {kind}"
    if item["femme"]:
        head += " f"
    src = item["src"].replace("\r", " ").replace("\n", " ")
    dst = item["dst"].replace("\r", " ").replace("\n", " ")
    return f"{head} | {src} | {dst}"


def learned_key(item):
    return (
        item["lang"],
        item["direction"],
        bool(item["femme"]),
        item["src"].casefold(),
    )
LAUGH_RE = re.compile(r"^[aehio]+$", re.IGNORECASE)
YESNO_WRAP_RE = re.compile(r"^([^\w]*)(\w+)([^\w]*)$", re.UNICODE)
# Smileys / <3 en fin de ligne : on les retire pour matcher PHRASES, puis on
# les recolle. Sinon "tk for me <3" rate le match exact.
CHAT_TAIL_RE = re.compile(
    r"(?:\s*(?:"
    r"<3|"
    r"\.:\(|"
    r">?:-?[()DpP3/\\]+|"
    r"[:;=8xX]-?[()DpP3/\\]+|"
    r"xD|"
    r"x\)|"
    r";\)|"
    r"\^\^|"
    r"[!?.,~]+"
    r"))+\s*$",
    re.IGNORECASE,
)
WAVE_RE = re.compile(r"^(o/|/o|\\o)\s+(\S+)$", re.IGNORECASE)
YOU_RE = re.compile(
    r"\b(you|you're|youre|u|ur|ya|dude|man|mate|bro)\b", re.IGNORECASE
)
# Lettres turques : une sortie vers le FR qui en ajoute, c'est une invention.
TR_CHARS_RE = re.compile(r"[ıİşŞğĞüÜöÖçÇ]")
# Pronoms / mini-reponses : seul cas ou l'historique aide. Le reste, le
# contexte CTF fait inventer un flag ou changer de langue.
CONTEXT_NEED = {
    "it", "that", "this", "them", "him", "her", "he", "she", "they",
    "same", "too", "also", "coming", "why", "who", "what",
}

# Mots-outils : servir a reconnaitre une ligne DEJA dans la langue cible,
# avant d'appeler le modele (sinon il "rephrase" du francais en francais).
FR_HINTS = {
    "je", "tu", "il", "elle", "on", "nous", "vous", "ils", "elles",
    "le", "la", "les", "un", "une", "des", "du", "de", "au", "aux",
    "et", "est", "pas", "que", "qui", "dont", "dans", "pour", "avec",
    "sur", "mais", "donc", "alors", "ce", "cet", "cette", "ces", "ca", "ça",
    "ne", "ni", "si", "ou", "où", "car", "mon", "ma", "mes", "ton", "ta",
    "tes", "son", "sa", "ses", "me", "te", "se", "y", "en",
    "suis", "es", "sommes", "etes", "êtes", "sont", "ai", "as", "avons",
    "fait", "fais", "va", "vas", "vais", "allez", "vont",
    "plus", "bien", "trop", "rien", "tout", "tous", "toute", "comme",
    "tres", "très", "aussi", "encore", "deja", "déjà", "meme", "même",
    "salut", "oui", "non", "ouais", "merci", "quoi", "comment", "pourquoi",
    "fini", "joue", "seul", "seule", "peine", "plaisir", "regarde",
    "continuer", "durer", "attends", "attendez", "allez", "bon", "coucou",
}
# "en"/"de"/"la" existent aussi en espagnol : un seul de ces mots ne veut
# PAS dire que la ligne est deja en francais (sinon "en mi caballo" est saute).
FR_STRONG = {
    "je", "nous", "vous", "ils", "elles", "est", "pas", "ça", "ca",
    "oui", "ouais", "merci", "salut", "suis", "avec", "dans", "pour",
    "mais", "donc", "alors", "quoi", "comment", "pourquoi", "fini",
    "cest", "c'est",
}
EN_HINTS = {
    "the", "you", "are", "is", "this", "that", "what", "how", "where",
    "when", "why", "who", "your", "have", "was", "were", "will", "can",
    "just", "not", "and", "but", "with", "from", "they", "she", "his",
    "her", "hey", "hi", "hello", "please", "thanks", "sorry", "don't",
    "dont", "can't", "cant", "it's", "its", "i'm", "im", "you're", "youre",
    "right", "now", "good", "get", "got", "been", "doing", "going",
    "for", "of", "to", "play", "playing", "thank", "thx", "actually",
    "mean", "means", "school",
}

# Le jeu envoie un code court ("de", "pt"). Ecrit tel quel dans le prompt, un
# petit modele le confond parfois avec autre chose ; le nom complet, jamais.
# Une langue absente de cette table fonctionne quand meme : son code part tel
# quel, ce qui suffit aux modeles recents.
# Police Sauerbraten : latin (accents d'Europe centrale inclus) + cyrillique.
# Pas d'arabe, hanzi, kana, hangeul, grec, thai, devanagari, etc.
LANGNAMES = {
    "en": "English",    "fr": "French",     "de": "German",     "es": "Spanish",
    "it": "Italian",    "pt": "Portuguese", "ru": "Russian",    "pl": "Polish",
    "nl": "Dutch",      "sv": "Swedish",    "no": "Norwegian",  "da": "Danish",
    "fi": "Finnish",    "cs": "Czech",      "sk": "Slovak",     "hu": "Hungarian",
    "ro": "Romanian",   "bg": "Bulgarian",  "tr": "Turkish",    "uk": "Ukrainian",
    "sr": "Serbian",    "hr": "Croatian",   "sl": "Slovenian",  "et": "Estonian",
    "ca": "Catalan",    "id": "Indonesian",
}

# /trep : langue du dernier chat recu. Mots partages (de, la, no, a, to) exclus.
# Un 8B n'est appele que si ces listes ne departagent pas ET que la ligne est longue.
# En cas d'egalite (salut = fr ET ro), on prend la langue la plus courante en Sauer.
INCOMING_KEEP = 8
LANG_PRIORITY = (
    "en", "fr", "de", "es", "pt", "it", "ru", "pl", "nl", "sv", "no", "da",
    "cs", "tr", "uk", "hu", "ro", "bg", "sr", "hr", "sk", "fi", "sl", "et",
    "ca", "id",
)
CYRILLIC_RE = re.compile(r"[\u0400-\u04FF]")
LANG_HINTS = {
    "en": {
        "the", "you", "are", "is", "this", "that", "what", "how", "where",
        "when", "why", "who", "your", "have", "was", "were", "will", "can",
        "just", "not", "and", "but", "with", "from", "they", "hey", "hello",
        "please", "thanks", "sorry", "don't", "dont", "it's", "im", "i'm",
        "you're", "youre", "right", "now", "good", "got", "going", "mate",
        "dude", "fuck", "shit", "coming", "really", "about", "here", "there",
        "my", "me", "we", "yes", "ok", "know", "think", "want", "need",
    },
    "fr": {
        "je", "tu", "nous", "vous", "ils", "elles", "est", "pas", "ça", "ca",
        "oui", "ouais", "salut", "suis", "avec", "dans", "pour", "mais",
        "donc", "alors", "quoi", "comment", "pourquoi", "cest", "c'est",
        "les", "une", "des", "fait", "vais", "allez", "attends", "merci",
        "bonjour", "putain", "peux", "veux", "bien", "plus",
    },
    "de": {
        "ich", "du", "und", "der", "die", "das", "nicht", "ist", "ein",
        "eine", "mit", "auf", "den", "dem", "wir", "auch", "von", "bist",
        "geht", "hurensohn", "mitte", "flagge", "deutsch", "bitte", "danke",
        "oder", "noch", "schon", "mal", "geh", "komm",
    },
    "es": {
        "hola", "qué", "estoy", "estas", "está", "vamos", "gracias", "hijo",
        "joder", "mierda", "cojo", "tienes", "bueno", "porque", "también",
        "tambien", "ahora", "donde", "dónde", "hacer", "puede", "quiero",
        "claro", "ves", "los", "las", "una", "pero",
    },
    "it": {
        "ciao", "che", "non", "sono", "questo", "figlio", "puttana",
        "arrivo", "prendo", "grazie", "dove", "quando", "perche", "perché",
        "allora", "bene", "anche", "niente", "però", "pero", "siamo",
        "hai", "ho", "c'è", "questa", "stai", "sto", "sei", "mi", "ti",
        "gli", "della", "nel", "piace", "divertendo",
    },
    "pt": {
        "nao", "não", "voce", "você", "voces", "eles", "estao", "estão",
        "filho", "pega", "pego", "peguei", "indo", "obrigado", "obrigada",
        "porra", "tras", "trás", "vindo", "sim", "cara", "aqui", "aí",
        "jogo", "vamos", "então", "entao", "pra", "você", "tá", "estou",
        "minha", "meu", "tem", "isso", "agora", "eu",
    },
    "nl": {
        "ik", "het", "een", "niet", "van", "voor", "zijn", "naar", "ook",
        "maar", "als", "bij", "nog", "wel", "geen", "jij", "gaan", "doen",
        "dank", "alsjeblieft", "waarom", "misschien",
    },
    "pl": {
        "nie", "jest", "się", "sie", "czy", "jak", "tego", "tylko", "może",
        "moze", "przez", "gdzie", "dlaczego", "kurwa", "cześć", "czesc",
        "jestem", "możesz", "mozesh", "proszę", "prosze", "dobrze",
    },
    "ru": {
        "не", "что", "это", "как", "меня", "тебя", "просто", "когда",
        "только", "сейчас", "давай", "флаг", "сука", "блять", "иди",
        "привет", "почему", "можно", "хорошо", "да", "нет",
    },
    "uk": {
        "не", "що", "це", "як", "мене", "тебе", "просто", "коли", "тільки",
        "зараз", "привіт", "дякую", "чому", "можна", "добре",
    },
    "sv": {
        "jag", "och", "det", "att", "inte", "har", "för", "som", "på",
        "är", "om", "kan", "vill", "varför", "tack", "hej",
    },
    "no": {
        "jeg", "ikke", "det", "har", "for", "som", "på", "er", "kan",
        "hvorfor", "takk", "hei", "bare",
    },
    "da": {
        "jeg", "ikke", "det", "har", "som", "på", "er", "kan", "hvorfor",
        "tak", "hej", "ikke",
    },
    "cs": {
        "jsem", "jsou", "není", "neni", "proč", "proc", "díky", "ahoj",
        "kde", "když", "kdyz", "můžeš", "muzes",
    },
    "hu": {
        "nem", "igen", "vagy", "vagyok", "köszönöm", "koszonom", "miért",
        "miert", "helló", "csá",
    },
    "tr": {
        "değil", "degil", "ben", "sen", "evet", "hayır", "hayir", "merhaba",
        "neden", "teşekkür", "tesekkur", "var", "yok",
    },
    "ro": {
        "nu", "sunt", "este", "mulțumesc", "multumesc", "dece", "unde",
        "salut", "fă", "fa",
    },
}
# Lettres quasi uniques. Minuscules ; on compare en casefold.
DIACRITICS = (
    ("pl", "ąćęłńśźż"),
    ("cs", "ěřů"),
    ("sk", "ľĺŕ"),
    ("hu", "őű"),
    ("tr", "ğış"),
    ("pt", "ãõ"),
    ("es", "ñ"),
    ("de", "ß"),
    ("sv", "å"),
    ("no", "æø"),
    ("da", "æø"),
    ("fr", "œù"),
)

DETECT_PROMPT = """\
What language is this multiplayer game chat line? Reply with ONLY a 2-letter \
ISO 639-1 code from: en fr de es it pt ru pl nl sv no da fi cs sk hu ro bg \
tr uk sr hr sl et ca id. If the line is only jargon (gg, wp, mid, flag) or \
you cannot tell, reply xx. No punctuation, no name of the language.
"""


def langcode(lang):
    """"pt-BR" ou "PT_br" -> "pt". Le jeu peut envoyer n'importe quelle casse."""
    return lang.strip().lower().replace("_", "-").split("-")[0]


def langlabel(lang):
    return LANGNAMES.get(langcode(lang), lang.strip() or "English")


def is_femme(raw):
    return (raw or "").strip().lower() in (
        "f", "female", "femme", "w", "woman", "1", "true", "yes",
    )


class Translator:
    def __init__(self):
        self.cache = OrderedDict()
        self.history = []          # (speaker, text, t) ; capture() exclut la ligne courante
        self.incoming = []         # (speaker, text, t) ; derniers chats RECUS, pour /trep
        self.speaker_lang = {}     # nom.casefold() -> code 2 lettres (dernier, pour /trep)
        self.last_out_lang = ""
        self.last_src_lang = ""    # source de la ligne IN courante (scoreboard, plusieurs OK)
        self.history_at = 0.0
        self.lock = threading.Lock()
        self.gpu_lock = threading.Lock()
        self.stats = {"calls": 0, "cached": 0, "skipped": 0, "failed": 0, "taught": 0, "examples": 0}
        self.last_activity = time.time()
        self.warned = set()
        self.learned = OrderedDict()
        self.learned_mtime = None
        self.load_learned()

    @staticmethod
    def _tag(name):
        cleaned = re.sub(r"[<>\r\n]", "", name or "").strip()
        return (cleaned[:32] if cleaned else "?")

    def capture(self, speaker, text):
        """Retient la ligne pour plus tard, rend les precedentes (sans celle-ci)."""
        now = time.time()
        with self.lock:
            if self.history_at and now - self.history_at > HISTORY_IDLE:
                self.history.clear()
            ctx = [
                (spk, msg)
                for spk, msg, _ts in self.history
                if msg.casefold() != text.casefold()
            ][-CONTEXT_LINES:]
            self.history.append((speaker or "", text, now))
            del self.history[:-CONTEXT_LINES]
            self.history_at = now
            return ctx

    def context_only(self, text):
        """Historique pour un apercu : on ne retient PAS le brouillon."""
        now = time.time()
        with self.lock:
            if self.history_at and now - self.history_at > HISTORY_IDLE:
                return []
            return [
                (spk, msg)
                for spk, msg, _ts in self.history
                if msg.casefold() != text.casefold()
            ][-CONTEXT_LINES:]

    def note_incoming(self, speaker, text):
        """Derniers messages recus (dir=in), independants du contexte 3 lignes."""
        text = (text or "").strip()
        if not text:
            return
        now = time.time()
        with self.lock:
            if self.incoming and now - self.incoming[-1][2] > HISTORY_IDLE:
                self.incoming.clear()
            self.incoming.append((speaker or "", text, now))
            del self.incoming[:-INCOMING_KEEP]

    def remember_lang(self, speaker, code):
        code = langcode(code) if code else ""
        if not speaker or not code:
            return
        with self.lock:
            self.speaker_lang[speaker.casefold()] = code

    def lang_of(self, speaker):
        if not speaker:
            return None
        with self.lock:
            return self.speaker_lang.get(speaker.casefold())

    @staticmethod
    def strip_at(text):
        """Enleve un @pseudo en tete (mentions /to) avant de detecter la langue."""
        t = (text or "").strip()
        if t[:1] != "@":
            return t
        parts = t.split(None, 1)
        return parts[1] if len(parts) == 2 else ""

    def pick_last(self, last, lastspk, me):
        """Ligne a laquelle /trep repond : query du client, sinon l'historique recu."""
        me_cf = (me or "").casefold()
        candidates = []
        if last and last.strip():
            candidates.append((lastspk or "", last.strip()))
        now = time.time()
        with self.lock:
            for spk, msg, ts in reversed(self.incoming):
                if now - ts > HISTORY_IDLE:
                    break
                candidates.append((spk, msg))
        seen = set()
        fallback = ("", "")
        for spk, msg in candidates:
            key = (spk.casefold(), msg.casefold())
            if key in seen:
                continue
            seen.add(key)
            if spk and me_cf and spk.casefold() == me_cf:
                continue
            if not fallback[0] and msg:
                fallback = (msg, spk)
            if self.trivial(msg):
                continue
            return msg, spk
        return fallback

    def detect_script(self, text):
        """Cyrillique / lettres rares : gratuit, avant les listes de mots."""
        cyr = len(CYRILLIC_RE.findall(text))
        latin = len(re.findall(r"[A-Za-zÀ-ÿ]", text))
        if cyr >= 2 and cyr >= latin:
            if re.search(r"[іїєґІЇЄҐ]", text):
                return "uk"
            if re.search(r"[ђћљњџЂЋЉЊЏ]", text):
                return "sr"
            if re.search(r"[ыэёЫЭЁ]", text):
                return "ru"
            if re.search(r"[ъЪ]", text) and not re.search(r"[ыЫ]", text):
                return "bg"
            return "ru"
        folded = text.casefold()
        best, hits = None, 0
        for code, chars in DIACRITICS:
            n = sum(ch in folded or ch in text for ch in chars)
            if n > hits:
                best, hits = code, n
        return best if hits else None

    def detect_lang(self, text):
        """Code 2 lettres, ou None. Pas d'appel modele."""
        text = (text or "").strip()
        if not text or self.trivial(text):
            return None
        script = self.detect_script(text)
        words = set(re.findall(r"[^\W\d_]+", text.lower(), re.UNICODE))
        scores = {code: 0 for code in LANG_HINTS}
        if script and script in scores:
            scores[script] += 3
        elif script:
            return script
        for code, hints in LANG_HINTS.items():
            scores[code] += len(words & hints)
        ranked = sorted(scores.items(), key=lambda kv: kv[1], reverse=True)
        if not ranked or ranked[0][1] <= 0:
            return script
        winner, top = ranked[0]
        second = ranked[1][1] if len(ranked) > 1 else 0
        if top > second:
            return winner
        tied = [code for code, n in ranked if n == top]
        for code in LANG_PRIORITY:
            if code in tied:
                return code
        return tied[0]

    def parse_lang_code(self, raw):
        raw = THINK_RE.sub("", raw or "").strip()
        raw = THINK_TAG_RE.sub("", raw).strip()
        if not raw:
            return None
        line = raw.splitlines()[0].strip().lower()
        token = re.sub(r"[^a-z\-]", " ", line).split()
        token = token[0] if token else ""
        token = token.replace("_", "-").split("-")[0]
        if token in ("xx", "x", "unknown", "jargon"):
            return None
        if token in LANGNAMES:
            return token
        for code, name in LANGNAMES.items():
            if name.lower() == token or name.lower().startswith(token):
                return code
        return None

    def detect_lang_model(self, text):
        """Dernier recours : un appel court, pas de few-shot."""
        try:
            raw = self.call_model([
                {"role": "system", "content": DETECT_PROMPT},
                {"role": "user", "content": text},
            ])
        except Exception as exc:  # noqa: BLE001
            print(f"  detect langue echec : {exc}", file=sys.stderr)
            return None
        code = self.parse_lang_code(raw)
        if not code:
            print(f"  detect langue ignore: {text!r} brut={raw!r}")
        return code

    def _system(self, lang, direction="in", femme=False):
        system = SYSTEM_PROMPT.format(
            lang=langlabel(lang), glossary=" ".join(GLOSSARY)
        )
        if direction == "out":
            system += OUT_ADDENDUM.format(lang=langlabel(lang))
        if femme:
            extra = FEMME_OUT if direction == "out" else FEMME_IN
            system += extra.format(lang=langlabel(lang))
        return system

    # -- filtres bon marche, avant de deranger le modele ---------------------

    def trivial(self, text):
        """Vrai si la ligne ne merite pas un appel au modele."""
        if self._is_smash(text) or self._is_laugh(text):
            return True
        words = re.findall(r"[^\W\d_]+", text.lower(), re.UNICODE)
        if not words:
            return True
        # "b", "k", ":D" : une lettre, rien a traduire (et Qwen y colle /no_think)
        if len(words) == 1 and len(words[0]) == 1:
            return True
        if all(w in INTERJECTIONS for w in words):
            return True
        return all(w in GLOSSARY or w in FILLER for w in words)

    def _is_laugh(self, text):
        letters = "".join(re.findall(r"[a-zA-Z]", text))
        return len(letters) >= 3 and "h" in letters.lower() and bool(LAUGH_RE.match(letters))

    def _is_smash(self, text):
        """Frappe clavier (asdf;lkj) : le modele inventait un salut ou un au revoir."""
        compact = "".join(re.findall(r"[a-zA-Z]", text))
        if len(compact) < 8:
            return False
        if " " in text.strip() and ";" not in text:
            return False
        vowels = sum(c in "aeiouyAEIOUY" for c in compact)
        if ";" in text and vowels / len(compact) < 0.4:
            return True
        return len(compact) >= 12 and vowels / len(compact) < 0.22

    def yesno(self, text, lang):
        """Traduit un oui/non isole sans passer par le modele."""
        table = YESNO.get(langcode(lang))
        if not table:
            return None
        match = YESNO_WRAP_RE.fullmatch(text.strip())
        if not match:
            return None
        dest = table.get(match.group(2).casefold())
        if dest is None:
            return None
        if match.group(2).isupper():
            dest = dest.upper()
        return f"{match.group(1)}{dest}{match.group(3)}".strip()

    def slang(self, text, lang, femme=False):
        """Mots d'argot trop courts pour le modele (idk, rdy, doin)."""
        table = dict(SLANG.get(langcode(lang)) or {})
        if femme:
            table.update(SLANG_FEMME.get(langcode(lang)) or {})
        if not table:
            return None
        match = YESNO_WRAP_RE.fullmatch(text.strip())
        if not match:
            return None
        dest = table.get(match.group(2).casefold())
        if dest is None:
            return None
        return f"{match.group(1)}{dest}{match.group(3)}".strip()

    @staticmethod
    def _fold_phrase(text):
        text = text.replace("\u2019", "'").replace("\u2018", "'").replace("`", "'")
        return re.sub(r"\s+", " ", text.strip()).casefold()

    @staticmethod
    def split_chat_tail(text):
        text = text.strip()
        match = CHAT_TAIL_RE.search(text)
        if not match:
            return text, ""
        return text[: match.start()].rstrip(), match.group(0)

    def phrase(self, text, lang, femme=False):
        """Match exact, puis sans smiley de fin, puis 'pseudo + phrase'."""
        table = dict(PHRASES.get(langcode(lang)) or {})
        if femme:
            table.update(PHRASES_FEMME.get(langcode(lang)) or {})
        if not table:
            return None
        folded = {self._fold_phrase(src): dst for src, dst in table.items()}
        core, tail = self.split_chat_tail(text)
        needle = self._fold_phrase(core)
        dest = folded.get(needle)
        prefix = ""
        extra = ""
        if dest is None:
            parts = needle.split(" ", 1)
            if len(parts) == 2 and parts[1] in folded:
                dest = folded[parts[1]]
                prefix = core.split(None, 1)[0] + " "
        if dest is None:
            for src in sorted(folded, key=len, reverse=True):
                if " " not in src or len(src) < 8:
                    continue
                if needle.startswith(src + " "):
                    rest = needle[len(src) + 1 :]
                    if rest and " " not in rest:
                        dest = folded[src]
                        extra = " " + core.split(None, src.count(" ") + 1)[-1]
                        break
        if dest is None:
            return None
        if not dest or dest == SENTINEL:
            return SENTINEL
        return f"{prefix}{dest}{extra}{tail}"

    def wave(self, text, lang):
        """o/ pseudo = salut, sans traduire le nick (COW n'est pas une vache)."""
        if langcode(lang) != "fr":
            return None
        match = WAVE_RE.fullmatch(text.strip())
        if not match:
            return None
        return f"salut {match.group(2)}"

    @staticmethod
    def decode_typos(text):
        text = re.sub(r"\bi\s*dk\b", "idk", text, flags=re.I)
        text = re.sub(r"\busry\b", "sry", text, flags=re.I)
        return text

    def cache_get(self, key):
        with self.lock:
            if key in self.cache:
                self.cache.move_to_end(key)
                return self.cache[key]
        return None

    def cache_put(self, key, value):
        with self.lock:
            self.cache[key] = value
            self.cache.move_to_end(key)
            while len(self.cache) > CACHE_SIZE:
                self.cache.popitem(last=False)

    def _drop_src_unlocked(self, src):
        needle = src.casefold()
        for key in [k for k in self.cache if len(k) > 3 and k[3] == needle]:
            del self.cache[key]

    def load_learned(self):
        loaded = OrderedDict()
        mtime = None
        try:
            mtime = os.path.getmtime(EXAMPLES_FILE)
            with open(EXAMPLES_FILE, encoding="utf-8") as fh:
                for line in fh:
                    item = parse_learned_line(line)
                    if not item:
                        continue
                    loaded[learned_key(item)] = item
        except FileNotFoundError:
            mtime = None
        except OSError as exc:
            print(f"  exemples.txt illisible : {exc}", file=sys.stderr)
        with self.lock:
            self.learned = loaded
            self.learned_mtime = mtime
            self.stats["examples"] = len(self.learned)
        if loaded:
            print(f"  {len(loaded)} exemple(s) charges depuis exemples.txt")

    def maybe_reload(self):
        try:
            mtime = os.path.getmtime(EXAMPLES_FILE)
        except OSError:
            return
        if mtime != self.learned_mtime:
            self.load_learned()

    def _write_learned_unlocked(self):
        folder = os.path.dirname(EXAMPLES_FILE)
        if folder:
            os.makedirs(folder, exist_ok=True)
        tmp = EXAMPLES_FILE + ".tmp"
        with open(tmp, "w", encoding="utf-8", newline="\n") as fh:
            fh.write(LEARNED_HEADER)
            for item in self.learned.values():
                fh.write(format_learned_line(item) + "\n")
        os.replace(tmp, EXAMPLES_FILE)
        try:
            self.learned_mtime = os.path.getmtime(EXAMPLES_FILE)
        except OSError:
            self.learned_mtime = time.time()

    def learn(self, src, dst, lang, direction, femme=False, kind="ex"):
        """Enregistre une paire. kind=exact : lookup seul ; kind=ex : few-shot."""
        src = self.decode_typos((src or "").strip())
        dst = (dst or "").strip()
        src = src.replace("\r", " ").replace("\n", " ").replace("\t", " ").strip()
        dst = dst.replace("\r", " ").replace("\n", " ").replace("\t", " ").strip()
        if not src or not dst:
            raise ValueError("need original and correction")
        if len(src) > 500 or len(dst) > 500:
            raise ValueError("line too long")
        direction = (direction or "in").strip().lower()
        if direction not in ("in", "out"):
            raise ValueError("bad dir")
        lang = langcode(lang) or "fr"
        kind = (kind or "ex").strip().lower()
        if kind in ("exact", "x"):
            kind = "exact"
        else:
            kind = "ex"
        if dst.casefold() == src.casefold():
            dst = SENTINEL
        item = {
            "lang": lang,
            "direction": direction,
            "femme": bool(femme),
            "kind": kind,
            "src": src,
            "dst": dst,
        }
        key = learned_key(item)
        with self.lock:
            self.learned[key] = item
            self.learned.move_to_end(key)
            self._drop_src_unlocked(src)
            self._write_learned_unlocked()
            self.stats["examples"] = len(self.learned)
            return len(self.learned)

    def list_learned(self, lang, direction, femme=False):
        """Toutes les paires de cette langue/direction (pas seulement les 12
        du prompt). Plus recentes a la fin. Pas d'appel au modele."""
        self.maybe_reload()
        lang = langcode(lang)
        direction = (direction or "in").strip().lower()
        if direction not in ("in", "out"):
            direction = "in"
        out = []
        with self.lock:
            for item in self.learned.values():
                if item["lang"] != lang or item["direction"] != direction:
                    continue
                if item["femme"] and not femme:
                    continue
                out.append(dict(item))
        return out

    def forget(self, src, lang, direction, femme=False):
        """Oublie une paire (menu examples / forget). Pas d'Ollama."""
        src = self.decode_typos((src or "").strip())
        src = src.replace("\r", " ").replace("\n", " ").replace("\t", " ").strip()
        if not src:
            raise ValueError("need original")
        if len(src) > 500:
            raise ValueError("line too long")
        direction = (direction or "in").strip().lower()
        if direction not in ("in", "out"):
            raise ValueError("bad dir")
        lang = langcode(lang) or "fr"
        needle = src.casefold()
        keys = [
            (lang, direction, bool(femme), needle),
            (lang, direction, not bool(femme), needle),
        ]
        with self.lock:
            for key in keys:
                if key in self.learned:
                    del self.learned[key]
                    self._drop_src_unlocked(src)
                    self._write_learned_unlocked()
                    self.stats["examples"] = len(self.learned)
                    return True
            return False

    def learned_lookup(self, text, lang, direction, femme=False):
        """Traduction enseignee, ou None."""
        self.maybe_reload()
        lang = langcode(lang)
        direction = (direction or "in").strip().lower()
        needle = (text or "").casefold()
        key_f = (lang, direction, True, needle)
        key = (lang, direction, False, needle)
        with self.lock:
            item = self.learned.get(key_f) if femme else None
            if item is None:
                item = self.learned.get(key)
            if item is None:
                return None
            return item["dst"]

    def learned_pairs(self, lang, direction, femme=False):
        lang = langcode(lang)
        direction = (direction or "in").strip().lower()
        out = []
        with self.lock:
            for item in self.learned.values():
                if item["lang"] != lang or item["direction"] != direction:
                    continue
                if item["femme"] and not femme:
                    continue
                if (item.get("kind") or "ex") != "ex":
                    continue
                out.append((item["src"], item["dst"]))
        return out

    # -- appel au modele ----------------------------------------------------

    def examples(self, lang, direction="in", femme=False, text=""):
        """Exemples de style pour la langue cible, ou aucun si elle n'est pas
        decrite. Mieux vaut aucun exemple qu'un jeu ecrit dans une autre langue :
        le modele recopierait la langue des reponses qu'on lui montre, et
        traduirait vers le francais une demande d'allemand.
        FEMME_IN seulement si la ligne tutoye : sinon watzson/idm/sry
        recuperent 't'es morte' des exemples."""
        paires = list(FEWSHOT.get(langcode(lang)) or [])
        if femme:
            use_femme = direction == "out" or bool(YOU_RE.search(text or ""))
            if use_femme:
                extra = FEWSHOT_FEMME_OUT if direction == "out" else FEWSHOT_FEMME_IN
                paires.extend(extra.get(langcode(lang)) or [])
        paires.extend(self.learned_pairs(lang, direction, femme))
        if not paires and lang not in self.warned:
            self.warned.add(lang)
            print(f"  note : pas d'exemples de style pour '{lang}' ({langlabel(lang)}), "
                  f"seules les consignes sont utilisees. Pour un meilleur rendu, "
                  f"ajoute un bloc a FEWSHOT dans ce fichier.")
        return paires

    def build_messages(self, text, lang, speaker, direction, context=None, femme=False):
        messages = [{"role": "system", "content": self._system(lang, direction, femme)}]
        for src, dst in self.examples(lang, direction, femme, text):
            messages.append({"role": "user", "content": src})
            messages.append({"role": "assistant", "content": dst})
        if context:
            lines = [f"<{self._tag(spk)}> {msg}" for spk, msg in context]
            extra = CONTEXT_ADDENDUM.format(
                chat="\n".join(lines), lang=langlabel(lang)
            )
            if speaker:
                extra += f"The next message is from player: {self._tag(speaker)}\n"
            messages.append({"role": "user", "content": extra.strip()})
            messages.append({"role": "assistant", "content": "ok"})
        messages.append({"role": "user", "content": text})
        return messages

    def call_model(self, messages, temperature=None, max_tokens=None, think=False):
        if model_manager.is_loading():
            return ""
        url, model, key = model_manager.endpoint()
        if not url:
            return ""
        payload = {
            "model": model,
            "messages": messages,
            "stream": False,
            "temperature": TEMPERATURE if temperature is None else temperature,
            "max_tokens": MAX_TOKENS if max_tokens is None else max_tokens,
        }
        if DISABLE_THINKING and not key:
            payload["chat_template_kwargs"] = {"enable_thinking": bool(think)}

        data = json.dumps(payload).encode("utf-8")
        headers = {"Content-Type": "application/json"}
        if key:
            headers["Authorization"] = "Bearer " + key
        req = urllib.request.Request(url, data=data, headers=headers)
        try:
            with urllib.request.urlopen(req, timeout=120) as resp:
                body = json.loads(resp.read().decode("utf-8"))
        except urllib.error.HTTPError as e:
            # Un serveur qui refuse chat_template_kwargs : on retire et on retente.
            if e.code == 400 and "chat_template_kwargs" in payload:
                payload.pop("chat_template_kwargs", None)
                data = json.dumps(payload).encode("utf-8")
                req = urllib.request.Request(url, data=data, headers=headers)
                with urllib.request.urlopen(req, timeout=120) as resp:
                    body = json.loads(resp.read().decode("utf-8"))
            else:
                raise

        choices = body.get("choices") or []
        if not choices:
            return ""
        msg = choices[0].get("message") or {}
        content = msg.get("content")
        if content is None:
            return ""
        if isinstance(content, list):
            parts = []
            for piece in content:
                if isinstance(piece, dict):
                    parts.append(piece.get("text") or "")
                else:
                    parts.append(str(piece))
            return "".join(parts)
        return content

    def clean(self, raw, original):
        out = THINK_RE.sub("", raw).strip()
        out = THINK_TAG_RE.sub("", out).strip()
        out = THINK_SWITCH_RE.sub("", out).strip()
        out = THINK_SWITCH_FR_RE.sub("", out).strip()
        if not out:
            return ""
        out = out.splitlines()[0].strip()
        out = re.sub(r"^<[^>]+>\s*", "", out)

        # L'ordre compte : il faut retirer le preambule AVANT les guillemets,
        # sinon le guillemet ouvrant se retrouve colle a la traduction.
        out = re.sub(r"^(translation|traduction|translated)\s*:\s*", "", out, flags=re.I).strip()

        # On ne retire les guillemets que s'ils encadrent vraiment la ligne,
        # pour ne pas manger une apostrophe legitime en fin de message.
        for opening, closing in (('"', '"'), ("'", "'"), ("\u201c", "\u201d"), ("\u00ab", "\u00bb")):
            if len(out) >= 2 and out.startswith(opening) and out.endswith(closing):
                out = out[1:-1].strip()
                break

        out = THINK_SWITCH_RE.sub("", out).strip()
        out = THINK_SWITCH_FR_RE.sub("", out).strip()

        if not out or out == SENTINEL:
            return ""
        if out.casefold() == original.casefold():
            return ""
        if META_NOTE_RE.search(out):
            return ""
        return out

    def context_useful(self, text, ctx):
        """L'historique seulement pour 'it' / 'same' / 'him'. Sinon Qwen
        pique le flag ou la langue d'une ligne d'avant."""
        if not ctx:
            return []
        words = re.findall(r"[^\W\d_]+", text.lower(), re.UNICODE)
        if len(words) <= 2:
            return []
        if len(words) <= 6 and any(w in CONTEXT_NEED for w in words):
            return ctx
        return []

    def invented(self, out, original, lang):
        """True si la trad rajoute un flag, une mort, du turc, etc."""
        if not out:
            return False
        src = original.casefold()
        dst = out.casefold()
        if any(n in dst for n in ("flag", "drapeau")) and not any(
            r in src for r in ("flag", "drapeau")
        ):
            return True
        if re.search(r"\b(t[' ]es morte|je suis mort|tu es mort|t'es morte)", dst) and not re.search(
            r"\b(dead|die|died|dying|mort|morte|kill|killed|death)\b", src
        ):
            return True
        if "nazi" in dst and "nazi" not in src:
            return True
        if langcode(lang) != "tr" and TR_CHARS_RE.search(out) and not TR_CHARS_RE.search(
            original
        ):
            return True
        return False

    def looks_already_target(self, text, lang):
        """Vrai si le texte a l'air d'etre deja dans la langue demandee."""
        words = set(re.findall(r"[^\W\d_]+", text.lower(), re.UNICODE))
        if not words:
            return False
        code = langcode(lang)
        fr_n = len(words & FR_HINTS)
        en_n = len(words & EN_HINTS)
        if code == "fr":
            if len(words & FR_STRONG) >= 1 and en_n == 0:
                return True
            if fr_n >= 2 and fr_n > en_n:
                return True
            return False
        if code == "en":
            if en_n >= 2:
                return True
            if en_n >= 1 and fr_n == 0:
                return True
            return en_n > fr_n
        return False

    def looks_foreign(self, text, lang):
        """Vrai si on est assez sur que ce n'est PAS la langue cible."""
        words = set(re.findall(r"[^\W\d_]+", text.lower(), re.UNICODE))
        if not words:
            return False
        code = langcode(lang)
        fr_n = len(words & FR_HINTS)
        en_n = len(words & EN_HINTS)
        if code == "fr":
            return en_n > fr_n
        if code == "en":
            return fr_n > en_n
        return False

    def force_translate(self, text, lang, speaker="", context=None, femme=False, direction="in"):
        system = (
            f"Translate this game chat into {langlabel(lang)}. "
            "The line is often badly misspelled (ar=are, eu=you, "
            "rn=right now, seriors=serious). "
            "If it is ALREADY in "
            f"{langlabel(lang)} (including that language's slang), "
            "output exactly: = . "
            f"French is not English. Never output = for a line that is not "
            f"{langlabel(lang)}. "
            "Otherwise decode the meaning, then output ONLY the "
            "translation. Never copy a foreign source. "
            "Do not invent: no flag, death, or extra sentence "
            "unless it is in the source line."
        )
        if femme:
            if direction == "out":
                system += (
                    " The speaker is a woman. Use feminine first-person "
                    f"agreement in {langlabel(lang)}."
                )
            else:
                system += (
                    " The reader is a woman. Use feminine 2nd-person "
                    f"agreement in {langlabel(lang)}."
                )
        if context:
            lines = [f"<{self._tag(spk)}> {msg}" for spk, msg in context]
            system += CONTEXT_ADDENDUM.format(
                chat="\n".join(lines), lang=langlabel(lang)
            )
            if speaker:
                system += f"The new message is from player: {self._tag(speaker)}\n"
        return self.call_model([
            {"role": "system", "content": system},
            {"role": "user", "content": text},
        ])

    def translate(self, text, lang, speaker, direction="in", preview=False, femme=False, reply=False, last="", lastspk="", force=False):
        self.last_activity = time.time()
        self.last_src_lang = ""
        text = self.decode_typos(text.strip())
        if not text:
            return ""

        if reply:
            direction = "out"
            last_text, last_who = self.pick_last(last, lastspk, speaker)
            last_text = self.strip_at(last_text)
            guessed = self.detect_lang(last_text) if last_text else None
            src = "hints" if guessed else None
            if not guessed:
                guessed = self.lang_of(last_who)
                if guessed:
                    src = "joueur"
            if not guessed and last_text:
                nwords = len(re.findall(r"[^\W\d_]+", last_text, re.UNICODE))
                if nwords >= 4:
                    self.gpu_lock.acquire()
                    try:
                        guessed = self.detect_lang_model(last_text)
                    finally:
                        self.gpu_lock.release()
                    if guessed:
                        src = "modele"
            if guessed:
                lang = guessed
                self.remember_lang(last_who, guessed)
            self.stats["reply_lang"] = langcode(lang)
            self.stats["reply_from"] = last_who or ""
            if last_text:
                how = f" ({src})" if src else ""
                print(f"  /trep cible {langcode(lang)}{how} d'apres <{self._tag(last_who)}> {last_text}")

        self.last_out_lang = langcode(lang)

        if preview:
            ctx = self.context_only(text)
            direction = "out"
        elif force:
            # Ne pas recapter : la ligne est deja dans l'historique, ou c'est
            # un MOTD / [INFO] qui ne doit pas polluer le chat des joueurs.
            ctx = self.context_only(text)
        else:
            # Toujours retenir la ligne (meme "gg" / deja en francais) : c'est le
            # sujet des "ok" et "it" qui suivent. La ligne courante n'est pas dans ctx.
            ctx = self.capture(speaker, text)
            if direction == "in":
                self.note_incoming(speaker, text)
                guessed_in = self.detect_lang(text)
                if guessed_in:
                    self.remember_lang(speaker, guessed_in)
                    self.last_src_lang = guessed_in
                elif self.looks_already_target(text, lang):
                    self.remember_lang(speaker, lang)
                    self.last_src_lang = langcode(lang)
        # Un "oh" / "uhm" ne doit pas recuperer la phrase d'un autre joueur.
        # Une phrase complete non plus : le contexte CTF fait inventer un flag.
        words = re.findall(r"[^\W\d_]+", text.lower(), re.UNICODE)
        ctx = self.context_useful(text, ctx)
        self.stats["last_ctx"] = len(ctx)

        taught = self.learned_lookup(text, lang, direction, femme)
        if taught is not None:
            taught_skip = (not taught or taught == SENTINEL or taught.casefold() == text.casefold())
            if force and taught_skip:
                taught = None
            else:
                self.stats["taught"] = self.stats.get("taught", 0) + 1
                if taught_skip:
                    self.stats["skipped"] += 1
                    return ""
                return taught

        if not force and self.trivial(text):
            self.stats["skipped"] += 1
            return ""

        mapped = self.yesno(text, lang)
        if mapped is not None:
            self.stats["mapped"] = self.stats.get("mapped", 0) + 1
            return mapped

        # sry / i fell : le feminin 1re personne, c'est ELLE qui parle, pas
        # un autre joueur dont on ignore le genre.
        femme_1st = bool(femme) and direction == "out"

        mapped = self.slang(text, lang, femme_1st)
        if mapped is not None:
            self.stats["mapped"] = self.stats.get("mapped", 0) + 1
            return mapped

        mapped = self.phrase(text, lang, femme_1st)
        if mapped is not None:
            if not mapped or mapped == SENTINEL:
                self.stats["skipped"] += 1
                return ""
            self.stats["mapped"] = self.stats.get("mapped", 0) + 1
            return mapped

        mapped = self.wave(text, lang)
        if mapped is not None:
            self.stats["mapped"] = self.stats.get("mapped", 0) + 1
            return mapped

        if not force and self.looks_already_target(text, lang):
            self.stats["skipped"] += 1
            return ""

        key = (
            direction,
            langcode(lang),
            bool(femme),
            text.casefold(),
            tuple(msg.casefold() for _spk, msg in ctx),
        )
        if not force:
            hit = self.cache_get(key)
            if hit is not None:
                self.stats["cached"] += 1
                return hit

        # Un seul appel GPU a la fois. L'apercu attend son tour : un skip
        # renvoyait du vide, et le jeu prenait ca pour un resultat definitif.
        self.gpu_lock.acquire()
        try:
            if force:
                raw = self.force_translate(text, lang, speaker, None, femme, direction)
                out = self.clean(raw, text)
                if out and self.invented(out, text, lang):
                    print(f"  ignore (invente): {text!r} brut={raw!r}")
                    out = ""
            else:
                raw = self.call_model(
                    self.build_messages(text, lang, speaker, direction, ctx, femme)
                )
                out = self.clean(raw, text)
                made_up = bool(out) and self.invented(out, text, lang)
                skipped_eq = (not preview) and (not out) and (
                    (direction == "out" and not self.looks_already_target(text, lang))
                    or (direction != "out" and (
                        self.looks_foreign(text, lang)
                        or (len(words) >= 2 and not self.looks_already_target(text, lang))
                    ))
                )
                if made_up or skipped_eq:
                    why = "invente" if made_up else "modele a saute une phrase"
                    print(f"  retry ({why}): {text!r} brut={raw!r}")
                    raw = self.force_translate(
                        text, lang, speaker, None, femme, direction
                    )
                    out = self.clean(raw, text)
                    if out and self.invented(out, text, lang):
                        print(f"  ignore (invente): {text!r} brut={raw!r}")
                        out = ""
            if not out:
                print(f"  ignore: {text!r} brut={raw!r}")
            if out:
                self.cache_put(key, out)
            self.stats["calls"] += 1
            return out
        finally:
            self.gpu_lock.release()

    def warmup(self):
        """Charge le modele et le prefixe reel (FR recu + EN apercu/tsay)
        pour que la premiere ligne en jeu ne coute pas plusieurs secondes.
        Les deux prefixes femme aussi : case cochee = le chemin chaud.
        Dernier appel = incoming FR femme (le plus frequent en jeu) pour
        laisser ce prefixe dans l'unique slot GPU."""
        try:
            start = time.time()
            self.gpu_lock.acquire()
            try:
                self.call_model(self.build_messages(".", "en", "", "out"))
                self.call_model(self.build_messages(".", "en", "", "out", femme=True))
                self.call_model(self.build_messages(".", "fr", "", "in"))
                self.call_model(self.build_messages(".", "fr", "", "in", femme=True))
            finally:
                self.gpu_lock.release()
            return time.time() - start
        except Exception as exc:  # noqa: BLE001
            print(f"  echec du prechauffage : {exc}", file=sys.stderr)
            return None


TRANSLATOR = Translator()


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        pass  # on gere nos propres logs, plus lisibles

    def reply(self, code, body=b"", headers=None):
        self.send_response(code)
        self.send_header("Content-Type", "text/plain; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        if headers:
            for key, value in headers:
                self.send_header(key, value)
        self.end_headers()
        if body:
            try:
                self.wfile.write(body)
            except (ConnectionAbortedError, ConnectionResetError, BrokenPipeError, OSError):
                pass

    def do_learn(self, parsed):
        params = parse_qs(parsed.query)
        lang = params.get("lang", ["fr"])[0]
        direction = params.get("dir", ["in"])[0]
        femme = is_femme(params.get("genre", [""])[0])
        kind = params.get("kind", ["ex"])[0]
        length = int(self.headers.get("Content-Length", 0) or 0)
        if length <= 0 or length > 8000:
            self.reply(400, b"need original and correction")
            return
        raw = self.rfile.read(length).decode("utf-8", "replace")
        if "\n" not in raw:
            self.reply(400, b"need original and correction")
            return
        src, dst = raw.split("\n", 1)
        try:
            n = TRANSLATOR.learn(src, dst, lang, direction, femme, kind)
        except ValueError as exc:
            self.reply(400, str(exc).encode("utf-8"))
            return
        except Exception as exc:  # noqa: BLE001
            print(f"  ERREUR /learn : {exc}", file=sys.stderr)
            self.reply(503)
            return
        if VERBOSE:
            k = "exact" if (kind or "").strip().lower() in ("exact", "x") else "ex"
            tag = f"/tfix {langcode(lang)} {direction} {k}"
            if femme:
                tag += " f"
            print(f"  [{tag}] {src.strip()!r} -> {dst.strip()!r} ({n} paires)")
        self.reply(200, b"ok")

    def do_unlearn(self, parsed):
        params = parse_qs(parsed.query)
        lang = params.get("lang", ["fr"])[0]
        direction = params.get("dir", ["in"])[0]
        femme = is_femme(params.get("genre", [""])[0])
        length = int(self.headers.get("Content-Length", 0) or 0)
        if length <= 0 or length > 8000:
            self.reply(400, b"need original")
            return
        src = self.rfile.read(length).decode("utf-8", "replace")
        if "\n" in src:
            src = src.split("\n", 1)[0]
        try:
            found = TRANSLATOR.forget(src, lang, direction, femme)
        except ValueError as exc:
            self.reply(400, str(exc).encode("utf-8"))
            return
        except Exception as exc:  # noqa: BLE001
            print(f"  ERREUR /unlearn : {exc}", file=sys.stderr)
            self.reply(503)
            return
        if not found:
            self.reply(404, b"not found")
            return
        if VERBOSE:
            tag = f"/unlearn {langcode(lang)} {direction}"
            if femme:
                tag += " f"
            print(f"  [{tag}] {src.strip()!r} ({TRANSLATOR.stats.get('examples', 0)} exemples)")
        self.reply(200, b"ok")

    def do_examples(self, parsed):
        """Liste les paires enseignees. Pas de modele, pas de capture()."""
        params = parse_qs(parsed.query)
        lang = params.get("lang", ["fr"])[0]
        direction = params.get("dir", ["in"])[0]
        femme = is_femme(params.get("genre", [""])[0])
        items = TRANSLATOR.list_learned(lang, direction, femme)
        lines = [format_learned_line(it) for it in items]
        body = ("\n".join(lines) + ("\n" if lines else "")).encode("utf-8")
        nshot = sum(1 for it in items if (it.get("kind") or "ex") == "ex")
        nexact = len(items) - nshot
        headers = [
            ("X-Count", str(len(items))),
            ("X-Shots", str(nshot)),
            ("X-Exact", str(nexact)),
        ]
        if VERBOSE:
            tag = f"/examples {langcode(lang) or lang} {direction}"
            if femme:
                tag += " f"
            print(f"  [{tag}] {len(items)} paires")
        self.reply(200, body, headers)

    def do_models_get(self):
        self.reply(200, model_manager.snapshot().encode("utf-8"))

    def do_models_post(self, parsed):
        params = parse_qs(parsed.query)
        mid = (params.get("id", [""])[0] or "").strip()
        name = (params.get("name", [""])[0] or "").strip()
        try:
            if parsed.path == "/models/install":
                body = model_manager.start_install(mid, activate=True)
            elif parsed.path == "/models/activate":
                body = model_manager.activate_model(mid, name)
            elif parsed.path == "/models/delete":
                body = model_manager.delete_model(mid, name)
            elif parsed.path == "/models/clearapi":
                body = model_manager.clear_api()
            elif parsed.path == "/models/api":
                length = int(self.headers.get("Content-Length", 0) or 0)
                if length <= 0 or length > 8000:
                    self.reply(400, b"need API address and key")
                    return
                raw = self.rfile.read(length).decode("utf-8", "replace")
                lines = raw.split("\n", 2)
                url = lines[0].strip() if lines else ""
                model = lines[1].strip() if len(lines) > 1 else ""
                key = lines[2].strip() if len(lines) > 2 else ""
                body = model_manager.save_api(url, key, model)
            else:
                self.reply(404)
                return
        except ValueError as exc:
            self.reply(400, str(exc).encode("utf-8"))
            return
        except RuntimeError as exc:
            self.reply(409, str(exc).encode("utf-8"))
            return
        except Exception as exc:  # noqa: BLE001
            print(f"  ERREUR {parsed.path} : {exc}", file=sys.stderr)
            self.reply(503)
            return
        if VERBOSE:
            print(f"  [{parsed.path}] {mid or 'api'}")
        self.reply(200, body.encode("utf-8"))

    def do_update_get(self, parsed):
        params = parse_qs(parsed.query)
        self.reply(200, updater.handle_get(params).encode("utf-8"))

    def do_update_post(self, parsed):
        params = parse_qs(parsed.query)
        try:
            body = updater.handle_post(params)
        except ValueError as exc:
            self.reply(400, str(exc).encode("utf-8"))
            return
        except RuntimeError as exc:
            self.reply(409, str(exc).encode("utf-8"))
            return
        if VERBOSE:
            print(f"  [/update] {params.get('action', ['(ancien client)'])[0]}")
        self.reply(200, body.encode("utf-8"))

    def do_assistant(self):
        """Assistant des reglages : le jeu envoie la question et la partie du
        catalogue des reglages qui lui correspond. On renvoie le texte du
        modele tel quel ; c'est le JEU qui verifie chaque ligne CMD: et qui ne
        l'execute qu'apres un clic du joueur."""
        TRANSLATOR.last_activity = time.time()
        try:
            length = int(self.headers.get("Content-Length", 0) or 0)
            req = json.loads(self.rfile.read(length).decode("utf-8", "replace") if length else "{}")
        except (ValueError, OSError):
            self.reply(400, "bad request".encode("utf-8"))
            return
        question = str(req.get("question") or "").strip()[:400]
        think = bool(req.get("think"))
        catalog = str(req.get("catalog") or "")[:9000]
        history = [str(h)[:1500] for h in (req.get("history") or [])][-4:]
        if not question:
            self.reply(400, "empty question".encode("utf-8"))
            return
        if model_manager.is_loading():
            self.reply(503, "The AI model is still loading, try again in a moment.".encode("utf-8"))
            return
        url, _model, _key = model_manager.endpoint()
        if not url:
            self.reply(503, "No AI model installed: Settings > Translation > Model.".encode("utf-8"))
            return
        try:
            prompt = (paths.CODE / "assistant-prompt.txt").read_text(encoding="utf-8")
        except OSError:
            prompt = "You are the settings assistant of the game. Propose game settings as lines starting with CMD:."
        system = assistant_system(prompt, catalog, question, history, assistant_menu_plan())
        messages = [{"role": "system", "content": system}]
        for i in range(0, len(history) - 1, 2):
            messages.append({"role": "user", "content": history[i]})
            messages.append({"role": "assistant", "content": history[i + 1]})
        # Rappel colle a la question : un petit modele oublie vite la langue
        # et la phrase d'accompagnement.
        words = set(re.findall(r"[^\W\d_]+", question.lower(), re.UNICODE))
        fr = len(words & ASSIST_FR)
        en = len(words & ASSIST_EN)
        if en > fr:
            hint = "(Reply in English, in natural sentences, before any CMD line.)"
        elif fr > en:
            hint = "(Réponds en français, avec au moins une phrase naturelle avant toute ligne CMD.)"
        else:
            hint = "(Reply in the language of this message, with at least one natural sentence before any CMD line.)"
        messages.append({"role": "user", "content": question + "\n\n" + hint})
        started = time.time()
        try:
            raw = self.server_call(messages, think)
        except Exception as exc:  # modele injoignable, delai depasse...
            print(f"  ERREUR assistant : {exc}", file=sys.stderr)
            self.reply(503, "The AI model did not answer.".encode("utf-8"))
            return
        out = THINK_RE.sub("", raw or "")
        out = THINK_TAG_RE.sub("", out).strip()
        # la police du jeu n'a pas les guillemets / apostrophes typographiques
        for fancy, plain in (("’", "'"), ("‘", "'"), ("“", '"'), ("”", '"'), ("…", "..."), ("–", "-"), ("—", "-"), (" ", " "), (" ", " ")):
            out = out.replace(fancy, plain)
        if not out:
            self.reply(503, "The AI model gave no answer, try again.".encode("utf-8"))
            return
        ncmd = sum(1 for line in out.splitlines() if line.strip().upper().startswith("CMD:"))
        print(f"  [assistant] {time.time() - started:.1f}s, {len(question)} car., {ncmd} commande(s){', reflexion' if think else ''}")
        self.reply(200, out.encode("utf-8"))

    @staticmethod
    def server_call(messages, think=False):
        if not think:
            return TRANSLATOR.call_model(messages, temperature=0.2, max_tokens=400)
        # Reflexion : il faut de la place pour penser PUIS repondre dans le
        # contexte du modele (4096 par defaut). ~3,5 caracteres par jeton.
        used = sum(len(m.get("content") or "") for m in messages) // 3 + 100
        room = max(600, min(2000, llama_ctx() - used))
        raw = TRANSLATOR.call_model(messages, temperature=0.6, max_tokens=room, think=True)
        answer = THINK_TAG_RE.sub("", THINK_RE.sub("", raw or "")).strip()
        if answer and "<think>" not in (raw or "").lower().split("</think>")[-1]:
            return raw
        # Reflexion trop longue, coupee avant la reponse : on repond sans.
        print("  [assistant] reflexion trop longue, reponse sans reflexion")
        return TRANSLATOR.call_model(messages, temperature=0.2, max_tokens=400)

    def do_health(self):
        info = {
            "ok": 1,
            "product": "l4zy",
            "installed": updater.installed_version(),
            "python": sys.executable,
            "python_version": sys.version.split()[0],
            "sys_path": sys.path,
            "flags_isolated": getattr(sys.flags, "isolated", 0),
            "pid": os.getpid(),
            "llama_port": llama_runtime.port(),
            "llama_mode": llama_runtime.mode(),
            "llama_pid": getattr(llama_runtime._proc, "pid", 0) if llama_runtime._proc else 0,
            "data_dir": str(paths.DATA),
            "models_dir": str(paths.MODELS),
            "examples": TRANSLATOR.stats.get("examples", 0),
        }
        self.reply(200, json.dumps(info, indent=1).encode("utf-8"))

    def do_shutdown(self, parsed):
        params = parse_qs(parsed.query)
        token = (params.get("token", [""])[0] or "")
        if not SHUTDOWN_TOKEN or token != SHUTDOWN_TOKEN:
            self.reply(403)
            return
        self.reply(200, b"bye")
        print("  arret demande par le lanceur.")
        threading.Thread(target=STOP_ALL[0], daemon=True).start()

    def do_POST(self):
        parsed = urlparse(self.path)
        if parsed.path == "/shutdown":
            self.do_shutdown(parsed)
            return
        if parsed.path == "/learn":
            self.do_learn(parsed)
            return
        if parsed.path == "/unlearn":
            self.do_unlearn(parsed)
            return
        if parsed.path.startswith("/models"):
            self.do_models_post(parsed)
            return
        if parsed.path == "/update":
            self.do_update_post(parsed)
            return
        if parsed.path == "/assistant":
            self.do_assistant()
            return
        if parsed.path != "/translate":
            self.reply(404)
            return

        params = parse_qs(parsed.query)
        lang = params.get("lang", ["fr"])[0]
        speaker = params.get("speaker", [""])[0]
        direction = params.get("dir", ["in"])[0]
        preview = params.get("preview", ["0"])[0] in ("1", "true", "yes")
        femme = is_femme(params.get("genre", [""])[0])
        reply = params.get("reply", ["0"])[0] in ("1", "true", "yes")
        force = params.get("force", ["0"])[0] in ("1", "true", "yes")
        board = params.get("board", ["0"])[0] in ("1", "true", "yes")
        last = params.get("last", [""])[0]
        lastspk = params.get("lastspk", [""])[0]
        if direction == "preview":
            preview = True
            direction = "out"
        if reply:
            direction = "out"

        length = int(self.headers.get("Content-Length", 0) or 0)
        text = self.rfile.read(length).decode("utf-8", "replace") if length else ""

        if board:
            TRANSLATOR.last_activity = time.time()
            body = TRANSLATOR.strip_at(text.strip())
            guessed = TRANSLATOR.detect_lang(body) if body else None
            if not guessed and body and TRANSLATOR.looks_already_target(body, lang):
                guessed = langcode(lang)
            if guessed:
                TRANSLATOR.remember_lang(speaker, guessed)
            if VERBOSE:
                print(f"  [board] {speaker or '?'}: {text.strip()} -> {guessed or '?'}")
            headers = [("X-Src-Lang", guessed)] if guessed else None
            self.reply(200, b"", headers)
            return

        start = time.time()
        try:
            out = TRANSLATOR.translate(
                text, lang, speaker, direction, preview=preview, femme=femme,
                reply=reply, last=last, lastspk=lastspk, force=force,
            )
        except Exception as exc:  # noqa: BLE001
            TRANSLATOR.stats["failed"] += 1
            print(f"  ERREUR backend : {exc}", file=sys.stderr)
            self.reply(503)
            return

        used_lang = TRANSLATOR.last_out_lang or langcode(lang)
        elapsed = (time.time() - start) * 1000
        if VERBOSE:
            arrow = f"-> {out}" if out else "(rien a traduire)"
            if force:
                tag = f"force recu->{used_lang}"
            elif reply:
                tag = f"/trep->{used_lang}"
            elif preview:
                tag = f"apercu->{used_lang}"
            elif direction == "out":
                tag = f"/tsay->{used_lang}"
            else:
                tag = f"recu->{used_lang}"
            if femme:
                tag += " f"
            nctx = TRANSLATOR.stats.get("last_ctx", 0)
            extra = f"  ctx={nctx}" if nctx else ""
            print(f"[{elapsed:6.0f} ms] [{tag}] {speaker or '?'}: {text}  {arrow}{extra}")

        headers = []
        if reply:
            headers.append(("X-Lang", used_lang))
        src = TRANSLATOR.last_src_lang
        if src:
            headers.append(("X-Src-Lang", src))
        self.reply(200, out.encode("utf-8"), headers or None)

    def do_GET(self):
        parsed = urlparse(self.path)
        if parsed.path == "/examples":
            self.do_examples(parsed)
            return
        if parsed.path == "/models":
            self.do_models_get()
            return
        if parsed.path == "/update":
            self.do_update_get(parsed)
            return
        if parsed.path == "/health":
            self.do_health()
            return
        if parsed.path != "/stats":
            self.reply(404)
            return
        body = json.dumps(TRANSLATOR.stats, indent=2).encode("utf-8")
        self.reply(200, body)


class Server(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    # Pas de SO_REUSEADDR : sous Windows il laisserait deux services ecouter
    # le meme port. Un port pris doit echouer franchement.
    allow_reuse_address = False

    def service_actions(self):
        """Appele par serve_forever() entre deux connexions."""
        if not IDLE_EXIT_MIN:
            return
        if time.time() - TRANSLATOR.last_activity < IDLE_EXIT_MIN * 60:
            return
        print(f"\ninactif depuis {IDLE_EXIT_MIN} min, arret automatique.")
        # shutdown() attend la fin de serve_forever() : l'appeler depuis la
        # boucle elle-meme se bloquerait sur soi-meme, d'ou le thread.
        threading.Thread(target=self.shutdown, daemon=True).start()


def setup_output(logpath):
    """Sous Windows, pythonw.exe demarre sans console et sys.stdout vaut None :
    le moindre print planterait le service avant meme qu'il n'ecoute, sans
    rien afficher puisqu'il n'y a pas de console pour montrer l'erreur."""
    if logpath:
        # Le journal ne sert qu'a relire la session en cours ; on le repart a
        # zero plutot que de le laisser grossir indefiniment.
        mode = "w" if os.path.exists(logpath) and os.path.getsize(logpath) > 5_000_000 else "a"
        stream = open(logpath, mode, encoding="utf-8", buffering=1, errors="replace")
        sys.stdout = sys.stderr = stream
        print(f"\n=== session du {time.strftime('%d/%m/%Y %H:%M:%S')} ===")
        return
    if sys.stdout is None or sys.stderr is None:
        devnull = open(os.devnull, "w")
        if sys.stdout is None:
            sys.stdout = devnull
        if sys.stderr is None:
            sys.stderr = devnull
        return
    # Sans ca, les logs restent bloques dans le tampon quand la sortie est
    # redirigee vers un fichier : on ne voit rien en direct.
    sys.stdout.reconfigure(line_buffering=True)


SHUTDOWN_TOKEN = ""
STOP_ALL = [lambda: None]


def parse_args():
    p = argparse.ArgumentParser(description="Service L4ZY : traduction du chat et mises a jour")
    p.add_argument("--model", default=MODEL,
                   help=f"nom envoye a llama-server (defaut : {MODEL})")
    p.add_argument("--port", type=int, default=LISTEN_PORT,
                   help=f"port d'ecoute (defaut : {LISTEN_PORT})")
    p.add_argument("--log", metavar="FICHIER",
                   help="ecrire le journal dans un fichier au lieu de la console")
    p.add_argument("--idle-exit", type=int, default=IDLE_EXIT_MIN, metavar="MINUTES",
                   help="s'arreter apres N minutes sans demande (0 = jamais)")
    p.add_argument("--quiet", action="store_true",
                   help="ne pas afficher chaque traduction")
    p.add_argument("--llm-url", metavar="URL",
                   help="URL OpenAI-compat (defaut : d'apres runtime.cfg)")
    p.add_argument("--no-start-llm", action="store_true",
                   help="ne pas demarrer llama-server")
    # Donnes par L4ZY.exe : chaque installation a ses dossiers et son port.
    p.add_argument("--data-dir", help="reglages, corrections, cle API, journaux")
    p.add_argument("--models-dir", help="modeles GGUF installes")
    p.add_argument("--state-dir", help="etat de l'installation (mises a jour)")
    p.add_argument("--profile-dir", help="profil du jeu (information)")
    p.add_argument("--parent-pid", type=int, default=0,
                   help="s'arreter quand ce processus (le lanceur) disparait")
    p.add_argument("--token", default="", help="jeton exige par /shutdown")
    p.add_argument("--starter", default="", help="modele de depart a installer au premier lancement")
    return p.parse_args()


def main():
    global MODEL, LISTEN_PORT, VERBOSE, IDLE_EXIT_MIN, LLM_URL, EXAMPLES_FILE, SHUTDOWN_TOKEN

    args = parse_args()
    MODEL, LISTEN_PORT, IDLE_EXIT_MIN = args.model, args.port, args.idle_exit
    VERBOSE = VERBOSE and not args.quiet
    paths.configure(args.data_dir, args.models_dir, args.state_dir, args.profile_dir)
    setup_output(args.log or str(paths.LOGS / "service.log"))
    SHUTDOWN_TOKEN = args.token or os.environ.get("L4ZY_TOKEN", "")
    EXAMPLES_FILE = str(paths.DATA / "exemples.txt")
    TRANSLATOR.load_learned()

    cfg, cfg_dir = llama_runtime.load_config()
    model_manager.init(cfg, cfg_dir)
    using_api = model_manager.using_api()
    if args.llm_url:
        LLM_URL = args.llm_url.rstrip("/")

    print("Service L4ZY")
    print(f"  version : {updater.installed_version()}  canal {updater.channel()}")
    print(f"  python  : {sys.executable} {sys.version.split()[0]}")
    print(f"  donnees : {paths.DATA}")
    print(f"  modeles : {paths.MODELS}")
    print(f"  ecoute  : http://127.0.0.1:{LISTEN_PORT}/translate")
    if using_api:
        print("  modele  : API internet (choix explicite du joueur, le chat quitte ce PC)")

    httpd_holder = []

    def warmup_async():
        if using_api:
            print("  cle API : llama-server n'est pas lance.")
            return
        if args.no_start_llm:
            return
        try:
            started = llama_runtime.start_if_needed(
                cfg, cfg_dir, logpath=str(paths.LOGS / "llama.log")
            )
        except Exception as exc:  # noqa: BLE001
            print(f"  ATTENTION : llama-server : {exc}")
            model_manager._set_msg(model_manager.status_text())
            return
        if not started:
            model_manager._set_msg(model_manager.status_text())
            return
        print("  prechauffage du modele...", flush=True)
        delay = TRANSLATOR.warmup()
        if delay is None:
            print("  ATTENTION : le modele ne repond pas (voir llama.log).")
        else:
            print(f"  modele charge en {delay:.1f} s, pret.")
        model_manager._set_msg(model_manager.status_text())

    def stop_all():
        llama_runtime.stop()
        if httpd_holder:
            threading.Thread(target=httpd_holder[0].shutdown, daemon=True).start()

    STOP_ALL[0] = stop_all

    try:
        httpd = Server(("127.0.0.1", LISTEN_PORT), Handler)
    except OSError as exc:
        print(f"  ERREUR : port {LISTEN_PORT} indisponible ({exc})")
        return 2
    httpd_holder.append(httpd)

    # Mises a jour d'abord : elles ne dependent ni du modele ni de la traduction.
    updater.startup()
    model_manager.resume_download_if_any()
    if not using_api and not model_manager._active_gguf():
        model_manager.maybe_start_starter(args.starter)
    threading.Thread(target=warmup_async, daemon=True).start()

    if args.parent_pid:
        llama_runtime.watch_pid(args.parent_pid, stop_all)

    if IDLE_EXIT_MIN:
        print(f"  arret automatique apres {IDLE_EXIT_MIN} min sans message.")
    try:
        httpd.serve_forever(poll_interval=1.0)
    except KeyboardInterrupt:
        print("\narret.")
    finally:
        llama_runtime.stop()
        print("  service arrete.")
    return 0


if __name__ == "__main__":
    sys.exit(main() or 0)
