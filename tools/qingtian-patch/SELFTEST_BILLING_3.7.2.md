# SlashSubs 3.7.2 billing self-test

## Automated (Node, 14/14 PASS)
1. forceExpire → isActivated=false, expired=true
2. plans: Monthly $9.9 / Yearly $49
3. selected plan amount used by verify
4. debugUnlock yearly → activated, ~365 days
5. debugUnlock monthly → activated, ~30 days
6. live TronGrid verify without matching tx → pending, no false unlock

## Manual in Cursor after Reload
1. Settings → Subscription → **Test expire** → main UI locks, paywall shows Monthly/Yearly
2. Choose Yearly ($49) or Monthly ($9.9)
3. Real pay: transfer exact USDT-TRC20 to shown address → **Verify payment** (also auto-polls every 15s)
4. Or QA: **QA: Simulate paid unlock** / Settings **Simulate paid** → UI returns to normal
