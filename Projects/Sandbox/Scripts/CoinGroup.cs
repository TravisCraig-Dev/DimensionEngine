using System;
using System.Collections;
using System.Collections.Generic;
using Dimension;

namespace Game
{
    /// <summary>Put on a parent of coins (trigger colliders): the object tagged Player picks one up by touching it (it
    /// turns off with SetActive), and it comes back after RespawnSeconds, spinning fast and slowing down to its usual
    /// speed (its Sprite Animator). Listens to the children's trigger events while this script is enabled.</summary>
    public class CoinGroup : Behaviour
    {
        [Tooltip("Seconds until a collected coin comes back (0 = never)")] public float RespawnSeconds = 5.0f;
        [Tooltip("How many times its usual speed a coin spins when it comes back")] public float SpinIn = 4.0f;

        private readonly Dictionary<Entity, Action<Entity>> m_Listeners = new();
        private int m_Collected;

        public override void OnEnable()
        {
            foreach (Entity coin in Entity.Children)
            {
                Action<Entity> listener = other => OnTouched(coin, other);
                coin.TriggerEnter += listener;
                m_Listeners[coin] = listener;
            }
        }

        public override void OnDisable()
        {
            foreach (var (coin, listener) in m_Listeners) coin.TriggerEnter -= listener;
            m_Listeners.Clear();
        }

        private void OnTouched(Entity coin, Entity other)
        {
            if (!other.CompareTag("Player") || !coin.ActiveSelf) return;
            coin.SetActive(false);
            ++m_Collected;
            int left = 0;
            foreach (Entity c in Entity.Children) if (c.ActiveSelf) ++left;
            Debug.Log($"{coin.Name} collected ({m_Collected} so far, {left} left)");
            if (RespawnSeconds > 0.0f) StartCoroutine(Respawn(coin));
        }

        private IEnumerator Respawn(Entity coin)
        {
            yield return new WaitForSeconds(RespawnSeconds);
            if (!coin.IsValid) yield break;
            coin.SetActive(true);
            var anim = coin.GetComponent<SpriteAnimator>();
            if (anim == null) yield break;
            float usual = anim.FramesPerSecond;
            anim.Play();
            for (float t = 0.0f; t < 1.0f && coin.ActiveSelf; t += Time.DeltaTime)
            {
                anim.FramesPerSecond = usual * (SpinIn + (1.0f - SpinIn) * t);
                yield return null;
            }
            anim.FramesPerSecond = usual;
        }
    }
}
