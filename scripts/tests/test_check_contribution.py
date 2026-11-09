import importlib.util
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


script = Path(__file__).resolve().parents[1] / "check-contribution.py"
spec = importlib.util.spec_from_file_location("check_contribution", script)
policy = importlib.util.module_from_spec(spec)
spec.loader.exec_module(policy)


class ContributionTests(unittest.TestCase):
    def pr(self, ticket="LUNE-123", body=None):
        return {
            "head": {"ref": ticket, "repo": {"fork": True}},
            "body": body if body is not None else f"Ticket: {ticket}\r\n\r\nFix recovery.",
        }

    def test_maintainer_and_github_issue_tickets_are_accepted_from_forks(self):
        for ticket in ("ABC-123", "LUNE-299", "GH-42"):
            with self.subTest(ticket=ticket):
                self.assertEqual(policy.check_pull_request(self.pr(ticket), [f"{ticket} - Fix recovery"]), [])

    def test_invalid_branches_and_shell_input_are_rejected(self):
        for branch in ("feature/LUNE-123", "LUNE-123-description", "lune-123", "abc-123", "GH-0", "ABC-0123",
                       "GH-#42", "LUNE-123;echo unsafe"):
            with self.subTest(branch=branch):
                self.assertTrue(policy.check_pull_request(self.pr(branch), [f"{branch} - Change"]))

    def test_every_new_commit_must_match_the_branch_ticket(self):
        for subject in ("Other message", "LUNE-5 - Wrong ticket", "GH-123 - Wrong tracker", "LUNE-123 -   "):
            with self.subTest(subject=subject):
                self.assertTrue(policy.check_pull_request(self.pr(), ["LUNE-123 - Good", subject]))

    def test_body_must_name_the_branch_ticket(self):
        for body in ("", "Fix recovery.", "Ticket: ABC-123", "Ticket: LUNE-1234", "Ticket: GH-123"):
            with self.subTest(body=body):
                self.assertTrue(policy.check_pull_request(self.pr(body=body), ["LUNE-123 - Change"]))
        body = "Ticket: LUNE-123\nFollow-up: LUNE-124 and GH-7"
        self.assertEqual(policy.check_pull_request(self.pr(body=body), ["LUNE-123 - Change"]), [])

    def test_linear_links_are_rejected(self):
        link = "https://" + "linear" + ".app/team/issue/LUNE-123"
        self.assertTrue(policy.check_pull_request(self.pr(body="Ticket: LUNE-123\n" + link), ["LUNE-123 - Change"]))
        self.assertTrue(policy.check_pull_request(dict(self.pr(), title=link), ["LUNE-123 - Change"]))

    def test_local_inputs_and_build_outputs_are_blocked(self):
        for path in ("game/source.c", "release/game.NDS", "test/save.sav", "bios9.bin", ".env", "ops/state.tfstate.backup", ".aws/credentials"):
            with self.subTest(path=path):
                self.assertTrue(policy.check_paths([path]))
        self.assertEqual(policy.check_paths(["client/src/wonder_trade/network.c", "server/proto/connectivity.proto"]), [])

    def test_files_deleted_before_pr_head_are_still_checked(self):
        for renamed in (False, True):
            with self.subTest(renamed=renamed), tempfile.TemporaryDirectory(prefix="dswt-policy-") as temp:
                root = Path(temp)
                def git(*args):
                    subprocess.check_call([
                        "git", "-C", str(root), "-c", "user.name=Fixture",
                        "-c", "user.email=fixture@example.invalid", *args,
                    ], stdout=subprocess.DEVNULL)
                git("init", "--quiet")
                (root / "game").mkdir()
                initial = "original.c" if renamed else "game/local.c"
                (root / initial).write_text("synthetic local input\n")
                git("add", ".")
                git("commit", "--quiet", "-m", "LUNE-123 - Add synthetic input")
                if renamed:
                    git("mv", "original.c", "game/local.c")
                    git("commit", "--quiet", "-m", "LUNE-123 - Rename synthetic input")
                git("rm", "--quiet", "game/local.c")
                git("commit", "--quiet", "-m", "LUNE-123 - Remove synthetic input")
                result = subprocess.run(
                    ["python3", str(script)], cwd=root,
                    env=dict(os.environ, GITHUB_EVENT_NAME="push"), capture_output=True, text=True,
                )
                self.assertEqual(result.returncode, 1)
                self.assertIn("game/local.c", result.stderr)


if __name__ == "__main__":
    unittest.main()
