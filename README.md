## Go Neural Net Sandbox

This repo is currently a sandbox for personal experimentation in neural net training in Go. I haven't made any particular attempt to make the training pipeline usable by others, but if you're interested, the rough summary is:

   * You must have HDF5 installed for C++ (https://support.hdfgroup.org/HDF5/release/obtainsrc.html), as well as Tensorflow installed for Python 3.
   * Compile using "compile.sh" in writedata, which expects you to have h5c++ available. (yes, no makefiles or build system, very hacky).
   * Run the resulting "write.exe" on a directory of SGF files to generate an h5 file of preprocessed training data.
   * Run train.py using that h5 file to train the neural net.

See LICENSE for software license. License aside, informally, if do you successfully use any of the code or any wacky ideas about neural net structure explored in this repo in your own neural nets or to run any of your own experiments, I would to love hear about it and/or might also appreciate a casual acknowledgement where appropriate. Yay.

## Experimental Notes
You can see the implementations of the relevant neural net structures in "model.py", although I may adapt and change them as time goes on.

### Current results
Currently, the best neural nets I've been training from this sandbox have about 3.28 million parameters, arranged as a resnet with about 6 blocks with a main trunk of 192 channels, and a policy head, trained for about 64 million samples. On GoGoD games from 1995 to mid 2016, which is about 50000 games with about 10 million training samples, the validation cross-entropy loss achieved with this many parameters is about 1.66 nats, corresponding to a perplexity of exp(1.66) = 5.26. So the neural net has achieved about the same entropy on average as if on every move it could pin down the next move to uniformly within 5 or 6 possible moves. The top-1 prediction accuracy is about 51%.

I haven't tried running on the KGS dataset yet that most papers out there tend to test on. From the few papers that test on both GoGoD and KGS, it seems very roughly that KGS is a bit easier to predict, with accuracies about 4% higher than for the pro games than for KGS amateur games, so probably these results correspond to roughly a 55% top-1 accuracy on KGS, which I'll get around to testing eventually.

I also tried testing a recent neural net against GnuGo, choosing moves by sorting and then drawing the move based using a beta distribution to draw from [0,1] to draw the move, and out of 250 games, it lost only 3. So things are developing pretty well.

It's almost certainly the case that the prediction quality could be increased further simply by making the neural nets bigger and training much longer, but in the interests of actually getting to run experiments in a reasonable time on a limited budget (just two single-GPU machines on Amazon EC2), I've so far deliberately refrained from making the neural net much bigger or spending weeks optimizing any particular neural net. See table below for summary of results.

| Neural Net | Structure  | Parameters  | KGS Accuracy | GoGoD Accuracy | Training Steps | Vs GnuGo |
|------|---|---|---|---|---|
| [Clark and Stokey (2015)](https://arxiv.org/abs/1412.3409)  | CNN 8 layers | ~560000 |  44.4% | 41.1% | 147M | 87%
| [Maddison et al. (2015)](https://arxiv.org/abs/1412.6564) |  CNN 12 layers | ~2300000 |  55.2% |  | 685M x 50 + 82M | 97%
| [AlphaGoFanHui-192 (2016)](https://storage.googleapis.com/deepmind-media/alphago/AlphaGoNaturePaper.pdf) | CNN 13 layers | 3880489 | 55.4% | | 340M x 50
| [AlphaGoFanHui-256 (2016)](https://storage.googleapis.com/deepmind-media/alphago/AlphaGoNaturePaper.pdf) | CNN 13 layers | 6795881 | 55.9%
| [Darkforest (2016)](https://arxiv.org/abs/1511.06410) | CNN 12 layers  | 12329756  | 57.1%  |   | 128M | 99.7%
| [Cazenave (2017)](http://www.lamsade.dauphine.fr/~cazenave/papers/resnet.pdf) | ResNet 10 blocks | 12098304 | 55.5% | 50.7% | 70M
| [Cazenave (2017)](http://www.lamsade.dauphine.fr/~cazenave/papers/resnet.pdf) | ResNet 10 blocks | 12098304 | 58.2% | 54.6% | 350M
| [AlphaGoZero-20Blocks(2017)](https://deepmind.com/documents/119/agz_unformatted_nature.pdf) | ResNet 20 blocks | 22837864 | 60.4% | | >1000M?
| Current Sandbox | ResNet 4 Blocks + 2 Special Blocks | 3285048 | | TODO | 64M | 98.8%

Also, to get an idea of how top-1 accuracy and nats correlate, at least on GoGoD and at least for these kinds of neural nets, here's a table roughly showing the correspondence I observed as the neural nets in this sandbox gradually grew larger and improved.

| GoGoD Accuracy (Top 1) | GoGoD Cross Entropy Loss (nats) |
|-----|---|
| 34% | 2.73
| 37% | 2.50
| 40% | 2.31
| 45% | 1.95
| 47% | 1.85
| 49% | 1.77
| 51% | 1.67


### Special Ladder Residual Blocks
Experimentally, I've found that neural nets can easily solve ladders, if trained directly to predict ladders (i.e. identify all laddered groups, rather than predict the next move)! Apparently 3 or 4 residual blocks is sufficient to solve ladders extending out up to 10ish spaces, near the theoretical max that such convolutions can reach. Near the theoretical max, they start to get a bit fuzzy, such as being only 70% sure of a working ladder, instead of 95+%, particularly if the ladder maker or ladder breaker stone is near the edge of the 6-wide diagonal path that affects the ladder.

However, specially-designed residual blocks appear to significantly help such a neural net detect solve ladders that extend well beyond the reach of its convolutions, as well as make it much more accurate in deciding when a stone nearly at the edge of the path that could affect the ladder actually does affect the ladder. This is definitely not a "zero" approach because it builds in Go-specific structure into the neural net, but nonetheless, the basic approach I tried was to take the 19x19 board and skew it via manual tensor reshaping:

    1234          1234000
    5678    ->    0567800
    9abc          009abc0
    defg          000defg

Now, columns in the skewed board correspond to diagonals on the original board. Then:

   * Compute a small number C of "value" and "weight" channels from the main resnet trunk via 3x3 convolutions.
   * Skew all the channels.
   * Compute a cumulative sum (tf.cumsum) along the skewed columns of both value*weight and weight, and divide to obtain a cumulative moving average.
   * Also repeat with reverse-cumulative sums and skewing the other way, to obtain all 4 diagonal directions.
   * Unskew all the results to make them square again.
   * Concatenate all the resulting 4C channels and multiply by a 4CxN matrix where N is the number of channels in the main trunk to transform the results back into "main trunk feature space".
   * Also apply your favorite activation function and batch norm at appropriate ponts throughout the above.
   * Add the results as residuals back to the main resnet trunk.

In the event that many of the weights are near zero, this will have the effect of propagating information potentially very long distances across the diagonals. In practice, I applied an exp-based transform to the weight channel to make it behave like an exponentially-weighted moving average, to obtain the effect that ladders care mostly about the first stone or stones they hit, and not the stones beyond them, as well as a bias to try to make it easier for the neural net to put low weight on empty spaces to encourage long-distance propagation.

Adding such a residual block to the neural net appears to greatly help long-distance ladder solving! When I trained a neural net with this to identify laddered groups, it appeared to have decently accurate ladder solving in test positions well beyond the theoretical range that its convolutions could reach alone, and I'm currently investigating whether adding this special block into a policy net helps the policy net's predictions about ladder-related tactics.

##### Update (201802):
Apparently, adding this block into the neural net does not cause it to be able to learn ladders in a supervised setting. From playing around with things and digging into the data a little, my suspicion is that in supervised settings, whether a ladder works or not is too strongly correlated with whether it gets formed in the first place, and similarly for escapes due to ladder breakers formed later. So that unless it's plainly obvious whether the ladder works or not (the ladder-target experiments show this block makes it much easier, but it's still not trivial), the neural net fails to pick up on it. It's possible that in a reinforcement learning setting (e.g. Leela Zero), this would be different.

Strangely however, adding this block in *did* improve the loss, by about 0.015 nats at 10 epochs persisting to still a bit more than 0.01 nats at 25 epochs. I'm not sure exactly what the neural net is using this block for, but it's being used for something. Due to the bottlenecked nature of the block (I'm using only C = 6), it barely increases the number of parameters in the neural net, so this is a pretty surprising improvement in relative terms. So I kept this block in the net while moving on to later experiments, and I haven't gone back to testing further.

### Using Ladders as an Extra Training Target

In another effort to make the neural net understand ladders, I added a second "ladder" head to the neural net and forced the neural net to simultaneously predict the next move and to identify all groups that were in or could be put in inescapable atari.

Mostly, this didn't work so well. With the size of neural net I was testing (~4-5 blocks, 192 channels) I was unable to get the neural net to produce predictions of the ladders anywhere near as well as it did when it had the ladder target alone, unless I was willing to downweight the policy target in the loss function enough that it would no longer produce as-good predictions of the next move. However, with a small weighting on the ladder target, the neural net learned to produce highly correct predictions for a variety of local inescapable ataris, such as edge-related captures, throw-in tactics and snapbacks, mostly everything except long-distance ladders. Presumably due to the additional regularization, this improved the loss for the policy very slightly, bouncing around 0.003-0.008 nats around 10 to 20 epochs.

But unsurprisingly, simply adding the ladder feature directly as an input to the neural net appeared to dominate this, improving the loss very slightly further. Also, with the feature directly provided as an input to the neural net, the neural net was finally able to tease out enough signal to mostly handle ladders well. However, even with such a blunt signal, it still doesn't always handle ladders correctly! In test positions, sometimes it fails to capture stones in a working ladder, or fails to run from a failed ladder, or continues to run from a working ladder, etc. Presumably pro players would not get into such situations in the first place, so there is a lack of data on these situations. This suggests that a lot of these difficulties are due to the supervised-learning setting. I'm quite confident that in a reinforcement-learning setting more like the "Zero" training, but with ladders actually provided directly as an input feature, the neural net would rapidly learn to not make such mistakes.


### Global Pooled Properties
Starting from a purely-convolutional policy net, I noticed a pretty significant change in move prediction accuracy despite only a small increase in the number of trainable parameters when I added the following structure to the policy head. The intent is to allow the neural net to compute some "global" properties of the board that may affect local play.
   * Separately from the main convolutions that feed into the rest of the policy head, on the side compute C channels of 3x3 convolutions (shape 19x19xC).
   * Max-pool, average-pool, and stdev-pool these C channels across the whole board (shape 1x1x(3C))
   * Multiply by (3C)xN matrix where N is the number of channels for the convolutions for the rest of the policy head (shape 1x1xN).
   * Broadcast the result up to 19x19xN and add it into the 19x19xN tensor resuting from the main convolutions for the policy head.

The idea is that the neural net can use these C global max-pooled or average-pooled channels to compute things like "is there currently a ko fight", and if so, upweight the subset of the N policy channels that correspond to playing ko threat moves, or compute "who has more territory", and upweight the subset of the N channels that match risky-move patterns or safe-move-patterns based on the result.

Experimentally, that's what it does! I tried C = 16, and when visualizing the activations 19x19xC in the neural net in various posititions just prior to the pooling, I found it had chosen the following 16 global features, which amazingly were mostly all humanly interpretable:
   * Game phase detectors (when pooled, these are all very useful for distinguishing opening/midgame/endgame)
       * 1 channel that activated when near a stone of either color.
       * 1 that activated within a wide radius of any stone. (an is-it-the-super-early-opening detector)
       * 1 that activated when not near a strong or settled group.
       * 1 that activated near an unfinished territoral border, and negative in any settled territory by either side.
   * Last-move (I still don't understand why these are important to compute to be pooled, but clearly the neural net thought they were.)
       * 5 distinct channels that activated near the last move or the last-last move, all in hard-to-understand but clearly different ways
   * Ko fight detector (presumably used to tell if it was time to play a ko threat anywhere else on the board)
       * 1 channel that highlighted strongly on any ko fight that was worth anything.
   * Urgency or weakness detectors (presumably used to measure the global temperature and help decide things like tenukis)
       * 4 different channels that detected various aspects of "is moving in this region urgent", such as being positive near weak groups, or in contact fights, etc.
   * Who is ahead? (presumably used to decide to play risky or safe)
       * 1 channel that activated in regions that the player controlled
       * 1 channel that activated in regions that the opponent controlled

So apparently global pooled properties help a lot. I bet this could also be done as a special residual block earlier in the neural net rather than putting it only in the policy head.

##### Update (201802):

The original tests of global pooled properties was done when the neural net was an order of magnitude smaller than it is now (~250K params instead of ~3M params), so I did a quick test of removing this part of the policy head to sanity check if this was still useful. Removing it immediately worsened the loss by about 0.04 nats on the first several epochs. Generally, differences on the first few epochs tend to diminish as the neural net converges further, but I would still guess at least 0.01 to 0.02 nats of harm at convergence, so this was a large enough drop that I didn't think it worth the GPU time to run it any longer.

So this is still adding plenty of value to the neural net. From a Go-playing perspective, it's also fairly obvious in practice that these channels are doing work. In the worst case at least in ko fights, since the capture of an important ko very plainly causes the neural net to suggest ko-threat moves all over the board that it would otherwise never suggest, including ones too far away to be easily reached by successive convolutions. I find it interesting that I haven't yet found any other published architectures include such a structure in the neural net.

### Wide Low-Rank Residual Blocks:

Adding a single residual block that performs a 1x9 and then a 9x1 convolution (as well as in parallel a 9x1 and then a 1x9 convolution for symmetry, sharing weights) appears to decrase the loss slightly more than adding an additional ordinary block that does two 3x3 convolutions. The idea is that such a block makes it easier for the neural net to propagate certain kinds of information faster across distance, in the case where such information doesn't need to involve as-detail of nonlinear computations. For example, one might imagine this being used to propagate information about large-scale influence from walls.

However, there's the drawback that either this causes an horizontal-vertical asymmetry, if you don't do the convolutions in the other order of orientations as well, or else this block costs twice in performance as much as an ordinary residual block. I suspect it's possible to get a version that is more purely benefiting, but I haven't tried a lot of permutations on this idea yet, that's still an area to be explored.

### Parametric Relus

I found a moderate improvement when I tried using parametric relus (https://arxiv.org/pdf/1502.01852.pdf) instead of ordinary relus. Plus I found a very weird result about what alpha parameters it wants to choose for them. I haven't heard of anyone else using parametric relus for Go, I'm curious if this result replicates in anyone else's neural nets.

For essentially a competely negligible increase in the number of parameters of the neural net, this change seemed to noticeably increase the fitting ability of the neural net in some useful way. As far as I can tell, this was not simply due to something simple like having any problem of dead relus beforehand, ever since batch normalization was added, much earlier, all stats about the gradients and values in the inner layers have indicated that very few of the relus die during training. The improvement was on the order of 0.025 nats over the first 10 epochs, decaying a little to closer to 0.010 to 0.015 nats by 30 epochs, but still persistently and clearly being better.

This is supported by a strange observation: for the vast majority of the relus, it seems like the neural net wants to choose a negative value for alpha! That means that the resulting activation function is non-monotone. In one of the most recent nets, depending on the layer, the mean value of alpha for all the relus in a layer varies from around -0.15 to around -0.40, with standard deviation on the order of 0.10 to 0.15.

With one exception: the relus involved in the global pooled properties layer persistently choose positive alpha, with a mean of positive 0.30. If any layer were to be different, I'd expect it to be this one, since these values are used in a very different way than any other layer, being globally pooled before being globally rebroadcast. Still, it's interesting that there's such a difference.

For the vast majority of the relus though, as far as I can tell, the neural net actually does "want" the activation to be non-monotone. In a short test run where alpha was initialized to a positive value rather than 0, the neural net over the course of the first few epochs forced all the alphas to be negative mean again, except for the ones in the global pooled properties layer, and in the meantime, had a larger training loss, indicating that it was not fitting as well due to the positive alphas.

I'd be very curious to hear whether this reproduces for anyone else. For now, I've been keeping the parametric relus, since they do seem to be an improvement, although I'm quite mystified about why non-monotone functions are good here.

### Chain Pooling





