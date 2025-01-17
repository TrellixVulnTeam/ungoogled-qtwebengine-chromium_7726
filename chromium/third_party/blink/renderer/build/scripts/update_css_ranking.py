# This script is used to update the CSS ranking. The CSS ranking will affect
# the grouping of CSS properties in Computed Style.
# Usage: Run `python update_css_ranking.py` to update the default
#        CSS ranking file and API.
#        Run `python update_css_ranking.py <ranking_file>` to update
#        the ranking to another file with the default ranking API.
#        Run `python update_css_ranking.py <ranking_file> <ranking_api_link>`
#        to update the ranking from <ranking_api_link> API to <ranking_file>

import urllib2
import json
import sys
import cluster
import json5_generator
import math

CSS_RANKING_API = "http://www.ch40mestatus.qjz9zk/data/csspopularity"
CSS_RANKING_FILE = "../../core/css/css_properties_ranking.json5"
CSS_PROPERTIES = "../../core/css/css_properties.json5"
CONFIG_FILE = "../../core/css/css_group_config.json5"


def reformat_properties_name(css_properties):
    for i in range(len(css_properties)):
        if css_properties[i][:5] == "alias":
            css_properties[i] = css_properties[i][5:]
        if css_properties[i][:6] == "webkit":
            css_properties[i] = "-" + css_properties[i]


def update_css_ranking(css_ranking_file, css_ranking_api):
    """Create the css_properties_ranking.json5 for uses in Computed Style grouping

    Args:
        css_ranking_file: file directory to css_properties_ranking.json5
        css_ranking_api: url to CSS ranking api

    """
    css_ranking = json.loads(urllib2.urlopen(css_ranking_api).read())
    css_ranking_content = {"properties": {}, "data": []}
    css_ranking_content["data"] = [
        property_["property_name"] for property_ in sorted(
            css_ranking, key=lambda x: -float(x["day_percentage"]))
    ]

    reformat_properties_name(css_ranking_content["data"])

    with open(css_ranking_file, "w") as fw:
        fw.write(
            "// The popularity ranking of all css properties the first properties is the most\n"
        )
        fw.write(
            "// used property according to: https://www.ch40mestatus.qjz9zk/metrics/css/popularity\n"
        )
        json.dump(css_ranking_content, fw, indent=4, sort_keys=False)


def find_partition_rule(css_property_set,
                        all_properties,
                        n_cluster,
                        transform=lambda x: x):
    """Find partition rule for a set of CSS property based on its popularity

    Args:
        css_property_set: list of CSS properties and their popularity of form
                          [(css_property_name, popularity_score)..]
        n_cluster: number of cluster to divide the set into
        all_properties: all CSS properties and its score
        transform: data transform function to transform the popularity score,
                   default value is the identity function

    Returns:
        partition rule for css_property_set
    """
    _, cluster_alloc, _ = cluster.k_means(
        [transform(p[1]) for p in css_property_set], n_cluster=n_cluster)
    return [
        all_properties[css_property_set[i][0]]
        for i in range(len(cluster_alloc) - 1)
        if cluster_alloc[i] != cluster_alloc[i + 1]
    ] + [1.0]


def produce_partition_rule(config_file, css_ranking_api):
    """Find the partition rule for the groups and print them to config_file

    Args:
        config_file: the file to write the parameters to
        css_ranking_api: url to CSS ranking api

    """
    css_ranking = sorted(
        json.loads(urllib2.urlopen(css_ranking_api).read()),
        key=lambda x: -x["day_percentage"])
    total_css_properties = len(css_ranking)
    css_ranking_dictionary = dict(
        [(x["property_name"], x["day_percentage"] * 100) for x in css_ranking])
    css_ranking_cdf = dict(
        zip([x["property_name"] for x in css_ranking], [
            float(i) / total_css_properties
            for i in range(total_css_properties)
        ]))
    css_properties = json5_generator.Json5File.load_from_files(
        [CSS_PROPERTIES]).name_dictionaries

    rare_non_inherited_properties = sorted(
        [(x["name"].original, css_ranking_dictionary[x["name"].original])
         for x in css_properties
         if not x["inherited"] and x["field_group"] is not None and "*" in
         x["field_group"] and x["name"].original in css_ranking_dictionary],
        key=lambda x: -x[1])
    rare_inherited_properties = sorted(
        [(x["name"].original, css_ranking_dictionary[x["name"].original])
         for x in css_properties
         if x["inherited"] and x["field_group"] is not None and "*" in
         x["field_group"] and x["name"].original in css_ranking_dictionary],
        key=lambda x: -x[1])

    rni_properties_rule = find_partition_rule(
        rare_non_inherited_properties, css_ranking_cdf, n_cluster=3)

    ri_properties_rule = find_partition_rule(
        rare_inherited_properties,
        css_ranking_cdf,
        n_cluster=2,
        transform=lambda x: math.log(x + 10e-6))

    with open(config_file, 'w') as fw:
        fw.write(
            "// The grouping parameter is a cumulative distribution " \
            "over the whole set of ranked\n"
        )
        fw.write("// CSS properties.\n")
        json.dump({
            "parameters": {},
            "data": [{
                "name": "rare_non_inherited_properties_rule",
                "cumulative_distribution": rni_properties_rule
            },
                     {
                         "name": "rare_inherited_properties_rule",
                         "cumulative_distribution": ri_properties_rule
                     }]
        },
                  fw,
                  indent=4)


if __name__ == '__main__':
    assert len(sys.argv) < 4, "Too many parameters"

    if len(sys.argv) == 1:
        update_css_ranking(CSS_RANKING_FILE, CSS_RANKING_API)
        produce_partition_rule(CONFIG_FILE, CSS_RANKING_API)
    elif len(sys.argv) == 2:
        update_css_ranking(sys.argv[1], CSS_RANKING_API)
        produce_partition_rule(CONFIG_FILE, CSS_RANKING_API)
    elif len(sys.argv) == 3:
        update_css_ranking(sys.argv[1], sys.argv[2])
        produce_partition_rule(CONFIG_FILE, sys.argv[2])
