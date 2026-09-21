"use client";

import { Account, Client, Teams } from "appwrite";
import { useEffect, useState } from "react";

type Invitation = { teamId: string; membershipId: string; userId: string; secret: string; teamName: string };

const client = new Client()
  .setEndpoint(process.env.NEXT_PUBLIC_APPWRITE_ENDPOINT!)
  .setProject(process.env.NEXT_PUBLIC_APPWRITE_PROJECT_ID!);
const teams = new Teams(client);
const account = new Account(client);

export default function InvitePage() {
  const [invitation, setInvitation] = useState<Invitation | null>(null);
  const [busy, setBusy] = useState(false);
  const [accepted, setAccepted] = useState(false);
  const [needsPassword, setNeedsPassword] = useState(false);
  const [password, setPassword] = useState("");
  const [confirmPassword, setConfirmPassword] = useState("");
  const [error, setError] = useState("");

  useEffect(() => {
    const query = new URLSearchParams(window.location.search);
    const teamId = query.get("teamId") || "";
    const membershipId = query.get("membershipId") || "";
    const userId = query.get("userId") || "";
    const secret = query.get("secret") || "";
    if (!teamId || !membershipId || !userId || !secret) {
      setError("This invitation link is incomplete. Ask the farm owner to send another invitation.");
      return;
    }
    setInvitation({ teamId, membershipId, userId, secret, teamName: query.get("teamName") || "this farm" });
  }, []);

  async function accept() {
    if (!invitation) return;
    setBusy(true); setError("");
    try {
      await teams.updateMembershipStatus(invitation);
      window.history.replaceState({}, "", window.location.pathname);
      setAccepted(true);
      const current = await account.get();
      setNeedsPassword(!current.passwordUpdate);
    } catch (caught) {
      setError(caught instanceof Error ? caught.message : "Could not accept this invitation.");
    } finally { setBusy(false); }
  }

  async function savePassword() {
    if (password.length < 8 || password !== confirmPassword) {
      setError("Enter a password of at least 8 characters and confirm it.");
      return;
    }
    setBusy(true); setError("");
    try {
      await account.updatePassword({ password });
      setNeedsPassword(false);
      setPassword(""); setConfirmPassword("");
    } catch (caught) {
      setError(caught instanceof Error ? caught.message : "Could not set your password.");
    } finally { setBusy(false); }
  }

  return <main className="invite-page"><section className="panel invite-panel">
    <p className="eyebrow">Live Stocking · Farm team</p>
    <h1>{accepted ? "Invitation accepted" : "Join a farm"}</h1>
    <p className="lede">{accepted ? needsPassword ? "Set a password to sign in to the mobile app." : "You can now sign in to the mobile app and view this farm and its devices." : `You have been invited to join ${invitation?.teamName || "a farm"}.`}</p>
    {accepted && needsPassword && <form onSubmit={(event) => { event.preventDefault(); void savePassword(); }}>
      <label>New password<input type="password" value={password} onChange={(event) => setPassword(event.target.value)} minLength={8} required autoComplete="new-password" /></label>
      <label>Confirm password<input type="password" value={confirmPassword} onChange={(event) => setConfirmPassword(event.target.value)} minLength={8} required autoComplete="new-password" /></label>
      <div className="actions"><button disabled={busy}>{busy ? "Saving…" : "Set password"}</button></div>
    </form>}
    {error && <p className="inline-error" role="alert">{error}</p>}
    <div className="actions">
      {!accepted && invitation && <button type="button" onClick={() => void accept()} disabled={busy}>{busy ? "Accepting…" : "Accept invitation"}</button>}
      <a className="invite-link" href="/">Open Live Stocking</a>
    </div>
  </section></main>;
}
