import html
import json
from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[2]
REFUSALS = ("WonderTrade_Text_RefusedEgg", "WonderTrade_Text_RefusedMail", "WonderTrade_Text_RefusedGame",
            "WonderTrade_Text_RefusedIllegal", "WonderTrade_Text_RefusedName")
LANGUAGES = {"en_US": "English", "fr_FR": "French", "de_DE": "German", "it_IT": "Italian",
             "es_ES": "Spanish", "ja_JP": "Japanese", "ko_KR": "Korean"}
# Titles and labels are not shown in the message window.
NOT_MESSAGES = {"WonderTrade_Text_Title", "WonderTrade_Text_MenuLabel"}


def json_bank(path):
    messages = json.loads((ROOT / path).read_text())["messages"]
    return {message["id"]: {key: message[key] for key in LANGUAGES} for message in messages}


def gmm_bank(path):
    text = (ROOT / path).read_text()
    bank = {}
    for ident, body in re.findall(r'<row id="([^"]+)" index="\d+">(.*?)</row>', text, re.S):
        values = dict(re.findall(r'<language name="(\w+)">(.*?)</language>', body))
        bank[ident] = {key: html.unescape(values.get(name, "")).replace("\\n", "\n") for key, name in LANGUAGES.items()}
    return bank


BANKS = {
    "Platinum": json_bank("client/games/res/text/wonder_trade.json"),
    "HeartGold and SoulSilver": json_bank("client/games/files/msgdata/msg/msg_0829.json"),
    "Diamond and Pearl": gmm_bank("client/games/files-dp/msgdata/msg/narc_0624.gmm"),
}


class RefusalTextTests(unittest.TestCase):
    def test_every_game_explains_every_refusal_in_every_language(self):
        for game, bank in BANKS.items():
            for ident in REFUSALS:
                with self.subTest(game=game, message=ident):
                    self.assertIn(ident, bank)
                    for language, text in bank[ident].items():
                        self.assertEqual(text.count("\n"), 1, language)  # Two lines fit every game's window.
                        self.assertNotIn("'", text, language)  # The games' charsets only have ’.

    def test_refusal_lines_are_no_longer_than_the_games_other_messages(self):
        for game, bank in BANKS.items():
            for language in LANGUAGES:
                limit = max(len(line) for ident, values in bank.items() if ident not in NOT_MESSAGES and ident not in REFUSALS
                            for line in values[language].split("\n"))
                for ident in REFUSALS:
                    for line in bank[ident][language].split("\n"):
                        with self.subTest(game=game, language=language, line=line):
                            self.assertLessEqual(len(line), limit)


if __name__ == "__main__":
    unittest.main()
